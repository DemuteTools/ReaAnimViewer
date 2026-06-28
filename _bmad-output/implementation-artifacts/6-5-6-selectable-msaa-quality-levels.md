---
baseline_commit: c78562094fa0fa40be79325a999fc4e9e235e774
---

# Story 6.5.6: Selectable MSAA quality levels (offscreen resolve)

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

> **Pre-ship polish — the last Epic 6.5 story before Epic 7 (ReaPack release).**
> Antho validated Story 6.5.5 in-Reaper and the **MSAA on/off checkbox produced no visible difference**;
> he asked for a **games-style MSAA quality selector** (Off / 2× / 4× / 8×). This story **replaces the
> MSAA portion of 6.5.5** with the robust, games-standard approach: render the scene into an **offscreen
> multisample colour buffer** and **blit-resolve** it to the window. 6.5.5's Normal-maps toggle, FPS
> readout, and on-canvas load-failure indication are **unchanged**. See
> [sprint-change-proposal-2026-06-28-msaa.md](../planning-artifacts/sprint-change-proposal-2026-06-28-msaa.md).

## Story

As a sound designer,
I want to choose my MSAA level like in a video game (Off / 2× / 4× / 8×),
so that I can trade edge smoothness for frame rate to suit my machine.

## Acceptance Criteria

1. **Given** the **Performance** section of the tool menu (the frameless Dear ImGui hamburger from Story 6.5.3, with the Normal-maps / MSAA / FPS controls added in 6.5.5)
   **When** I open the **MSAA** control
   **Then** it is a **multi-level selector** — **Off / 2× / 4× / 8×**, each option **clamped to the GPU's maximum sample count** (`GL_MAX_SAMPLES`: an option above the max is hidden or disabled) — **not** a single on/off checkbox. [Source: epics.md#Story-6.5.6 lines 706-708; the 6.5.5 MSAA checkbox this replaces [viewer_window.cpp:664-678](../../src/viewer_window.cpp#L664-L678); the 4-way RadioButton rhythm to mirror = the Shadow selector [viewer_window.cpp:491-501](../../src/viewer_window.cpp#L491-L501)]

2. **Given** the selector
   **When** I change the level
   **Then** it takes effect **live** (jagged ↔ smooth silhouettes) with **no window/context recreation, no flicker, and no crash**. [Source: epics.md:708; the window/context is deliberately never recreated — file header [viewer_window.cpp:9-20](../../src/viewer_window.cpp#L9-L20), WM_SIZE "no recreation on resize" [viewer_window.cpp:715-724](../../src/viewer_window.cpp#L715-L724)]

3. **Given** the rendering pipeline
   **Then** MSAA is delivered by an **offscreen multisample colour buffer resolved (blit) to the window** — a level change **truly re-samples** and does **not** depend on `glEnable/glDisable(GL_MULTISAMPLE)` on the default framebuffer (which some drivers ignore — the root cause of the 6.5.5 "no visible difference"); the 6.5.5 **window multisample pixel-format bootstrap is reverted** (the window becomes single-sample; MSAA lives entirely in the offscreen FBO). [Source: sprint-change-proposal §1, §2; epics.md:709; the bootstrap to revert = `ChooseMultisamplePixelFormat` + the MS path in `CreateGLContextFor` [viewer_window.cpp:242-351](../../src/viewer_window.cpp#L242-L351); the per-frame toggle to remove [renderer.cpp:585-591](../../src/renderer.cpp#L585-L591)]

4. **Given** the offscreen buffer
   **Then** it is **(re)allocated only on a level change or window resize** (cold path) — the per-frame render path stays **allocation-free** (D2) — and **Off renders directly to the window** (framebuffer 0) with **no offscreen cost** (the MS targets are freed at Off). [Source: epics.md:710; D2 zero-alloc architecture.md:211-235; the 6.5.4 cold-path-realloc precedent = `AllocShadowMap`/`SetShadowQuality` [renderer.cpp:438-487](../../src/renderer.cpp#L438)]

5. **Given** the cast-shadow pass shipped in 6.5.4
   **Then** it still works at every MSAA level: the shadow depth pass binds its **own** depth FBO and, when done, **restores to the active scene target** — framebuffer 0 when MSAA is Off, the **offscreen MS FBO** when MSAA is on — **not** an unconditional framebuffer 0. The floor, grid, and PCF shadow render into the same target as the mesh. [Source: sprint-change-proposal §2 "the 6.5.4 shadow pass restores to the active scene target instead of FB0"; the line that must change [renderer.cpp:715](../../src/renderer.cpp#L715) `glBindFramebuffer(GL_FRAMEBUFFER, 0)`]

6. **Given** weak-hardware operation and the boundary discipline
   **Then** the change is **purely visual/perf** — it never touches transport, the playhead/scrub/clamp path, or persistence (`.rpp`/D9/[pcm_source_anim.cpp](../../src/pcm_source_anim.cpp) byte-for-byte unchanged — **session-only** state; a fresh viewer opens at the default level), and it **never crashes the host** (NFR-R1, AR17): **any FBO/renderbuffer allocation failure falls back non-fatally to a working no-MSAA (Off) view**. The per-frame path stays **allocation-free** (D2). AR15 register-symmetry is intact: **no** `rec->Register`, **no** new `REAPERAPI_WANT_*`. The change is confined to `src/renderer.{h,cpp}`, `src/viewer_window.cpp`, `src/gl_loader.h`, and `src/gpu_resources.h`. **No** `scene.h` / `CMakeLists.txt` / `plugin_main` / `reaper_api` / `pcm_source` / `asset_loader` change. [Source: AR15 epics.md:163, AR17 epics.md:165, AR18 epics.md:166, AR20 epics.md:171; sprint-change-proposal §2 file list]

7. **Given** ≥60 fps is the perf budget (NFR-P1)
   **Then** lower MSAA levels (and Off) hold ≥60 fps on Antho's machine; raising the level visibly smooths silhouettes (the point), trading frame time. [Source: NFR-P1; epics.md:701-702 "trade edge smoothness for frame rate"]

### Out of scope (explicitly deferred — do NOT implement here)

- **Persisting the MSAA level** across save/reopen. It is **session-only** like the camera / light / shadow / floor — a fresh viewer opens at the default. **No** `.rpp`/SaveState/D9 wiring; leave [pcm_source_anim.cpp](../../src/pcm_source_anim.cpp) byte-for-byte unchanged.
- **Changing the Normal-maps toggle, FPS readout, or on-canvas load-failure indication** from 6.5.5 — they stay exactly as shipped. This story touches **only** the MSAA control + its render path.
- **Supersampling (SSAA), FXAA/post-process AA, or per-pass AA.** MSAA via offscreen multisample resolve only.
- **An sRGB-aware resolve / GL_FRAMEBUFFER_SRGB.** The offscreen colour buffer is plain **`GL_RGBA8`** to match the window and today's resolve behaviour (see Dev Notes "Colour format — match the window, do NOT use sRGB").
- **A custom MSAA icon.** The Performance section already has Antho's `icon_performance` (6.5.5) — the selector lives inside it; no new icon.

## Tasks / Subtasks

- [x] **Task 1 — Add the renderbuffer + blit GL entry points and enums (`gl_loader.h`) (AC3/AC4/AC5).**
  - [x] Append to the `RAV_GL_FUNCS(X)` X-macro list (after `glCheckFramebufferStatus` [gl_loader.h:127](../../src/gl_loader.h#L127) — note that line currently has **no** trailing `\`; add the backslash and the new rows below it, keeping the last row backslash-free):
    `X(void, glGenRenderbuffers, (GLsizei, GLuint*))`,
    `X(void, glDeleteRenderbuffers, (GLsizei, const GLuint*))`,
    `X(void, glBindRenderbuffer, (GLenum, GLuint))`,
    `X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei))`,
    `X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))`,
    `X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))`.
  - [x] Add the matching `#define gl... rav_gl...` routing lines next to the existing FBO ones [gl_loader.h:166-170](../../src/gl_loader.h#L166-L170).
  - [x] Add the enums (guarded `#ifndef`, in the existing FBO-enum block near [gl_loader.h:58-86](../../src/gl_loader.h#L58)): `GL_RENDERBUFFER 0x8D41`, `GL_COLOR_ATTACHMENT0 0x8CE0`, `GL_READ_FRAMEBUFFER 0x8CA8`, `GL_DRAW_FRAMEBUFFER 0x8CA9`, `GL_MAX_SAMPLES 0x8D57`. (`GL_DEPTH_ATTACHMENT`, `GL_DEPTH_COMPONENT24`, `GL_RGBA8`, `GL_FRAMEBUFFER`, `GL_FRAMEBUFFER_COMPLETE`, `GL_COLOR_BUFFER_BIT`, `GL_NEAREST`, `GLbitfield` are already present.)
  - [x] **Remove** the now-unused `GL_MULTISAMPLE 0x809D` define [gl_loader.h:80-86](../../src/gl_loader.h#L80-L86) (it was 6.5.5's; the offscreen MS FBO multisamples implicitly — no enable bit). Append-only precedent for the new rows = 6.5.1/6.5.3/6.5.4. [Source: sprint-change-proposal §2 "gl_loader.h append renderbuffer + glBlitFramebuffer rows + enums"]

- [x] **Task 2 — `GpuRenderbuffer` RAII alias (`gpu_resources.h`) (AC4/AC6).**
  - [x] Add `struct RenderbufferDeleter { void operator()(GLuint h) const noexcept { glDeleteRenderbuffers(1, &h); } };` next to `FramebufferDeleter` [gpu_resources.h:49](../../src/gpu_resources.h#L49), and `using GpuRenderbuffer = GpuHandle<RenderbufferDeleter>;` next to `GpuFramebuffer` [gpu_resources.h:55](../../src/gpu_resources.h#L55). Same move-only `GpuHandle` body; destructor needs a current GL context (already the renderer's contract). [Source: gpu_resources.h pattern]

- [x] **Task 3 — Offscreen MSAA targets + resolve in the renderer (`renderer.h` / `renderer.cpp`) (AC2/AC3/AC4/AC5). READ Dev Notes "The offscreen-resolve render flow" + "The shadow-pass restore is the regression" FIRST.**
  - [x] **`renderer.h`:** replace the 6.5.5 `bool msaa_on_ = true;` member [renderer.h:217](../../src/renderer.h#L217) and the `SetMsaaEnabled` setter [renderer.h:124](../../src/renderer.h#L124) with: member `int msaa_samples_ = 4;` (default 4×, clamped at startup) and inline setter `void SetMsaaSamples(int s) { msaa_samples_ = s; }` (mirror `SetFloorVisible`). Add the owned targets + alloc cache (next to the shadow members [renderer.h:228-238](../../src/renderer.h#L228)): `GpuFramebuffer msaa_fbo_; GpuRenderbuffer msaa_color_rb_; GpuRenderbuffer msaa_depth_rb_; int msaa_alloc_w_ = 0, msaa_alloc_h_ = 0, msaa_alloc_samples_ = 0;`. Add private helper decl `bool AllocMsaaTargets(int w, int h, int samples);`. Update the class header comment (the 6.5.4 block at [renderer.h:11-17](../../src/renderer.h#L11)) to note the new **offscreen multisample COLOUR FBO + blit-resolve** sibling deviation.
  - [x] **`renderer.cpp` — remove the 6.5.5 toggle:** delete the per-frame `glEnable/glDisable(GL_MULTISAMPLE)` block [renderer.cpp:585-591](../../src/renderer.cpp#L585-L591).
  - [x] **`renderer.cpp` — `AllocMsaaTargets(w, h, samples)` (cold path):** gen/bind `msaa_color_rb_` → `glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h)`; gen/bind `msaa_depth_rb_` → `glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h)`; gen/bind `msaa_fbo_`, attach colour to `GL_COLOR_ATTACHMENT0` and depth to `GL_DEPTH_ATTACHMENT` via `glFramebufferRenderbuffer`; check `glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE`; **always restore `glBindFramebuffer(GL_FRAMEBUFFER, 0)` and `glBindRenderbuffer(GL_RENDERBUFFER, 0)` before returning**. On any failure return `false` (caller falls back to Off, AR17). On success set `msaa_alloc_{w_,h_,samples_}`. Use the `GpuRenderbuffer::addr()`/`GpuFramebuffer::addr()` + move-assign pattern from `AllocShadowMap` [renderer.cpp:438-460](../../src/renderer.cpp#L438).
  - [x] **`renderer.cpp` — `RenderFrame` reconcile (top, cold path only):** compute `int want = msaa_samples_;` (Off==0). If `want > 0` and (`msaa_alloc_w_ != width || msaa_alloc_h_ != height || msaa_alloc_samples_ != want || !msaa_fbo_.get()`) → call `AllocMsaaTargets(width, height, want)`; on failure set the alloc cache so it won't retry every frame (e.g. `msaa_alloc_samples_ = 0`) and treat as Off this session-change (AR17). If `want == 0` and targets exist → free them (`msaa_fbo_ = GpuFramebuffer(); msaa_color_rb_ = GpuRenderbuffer(); msaa_depth_rb_ = GpuRenderbuffer(); msaa_alloc_* = 0`) so Off has no offscreen cost (AC4). Then `const GLuint scene_fbo = (want > 0 && msaa_fbo_.get()) ? msaa_fbo_.get() : 0;`.
  - [x] **`renderer.cpp` — bind the scene target:** after the reconcile, `glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);` then the existing `glViewport(0,0,width,height)` + clear + passes. **Everything (clear, shadow restore, floor, mesh) targets `scene_fbo`.**
  - [x] **`renderer.cpp` — fix the shadow-pass restore (AC5):** change [renderer.cpp:715](../../src/renderer.cpp#L715) `glBindFramebuffer(GL_FRAMEBUFFER, 0);` → `glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);` (and update the inline comment — it no longer always goes to FB0). The shadow depth FBO bind/viewport is otherwise unchanged.
  - [x] **`renderer.cpp` — resolve at the end of `RenderFrame`:** after the visible mesh pass, **if `scene_fbo != 0`**: `glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo_.get()); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); glBlitFramebuffer(0,0,width,height, 0,0,width,height, GL_COLOR_BUFFER_BIT, GL_NEAREST);` then `glBindFramebuffer(GL_FRAMEBUFFER, 0);`. **Leave framebuffer 0 bound on return** so ImGui (drawn after, in `DrawToolUi`) and `SwapBuffers` land on the window over the resolved scene. The Off path already has FB0 bound throughout — no blit, identical to today.
  - [x] **`renderer.cpp` — `Shutdown`:** free the new targets (`msaa_fbo_`/`msaa_color_rb_`/`msaa_depth_rb_` are RAII, so a plain `= Gpu…()` reset, mirroring the shadow teardown [renderer.cpp:790-793](../../src/renderer.cpp#L790)).

- [x] **Task 4 — Multi-level selector UI + revert the 6.5.5 MSAA UI state (`viewer_window.cpp`) (AC1/AC2).**
  - [x] **Globals:** remove the 6.5.5 MSAA globals `g_msaa_on` / `g_msaa_available` / `g_msaa_samples` [viewer_window.cpp:167-174](../../src/viewer_window.cpp#L167-L174). Add `int g_msaa_level = 4;` (selected sample count: 0/2/4/8 — default 4×, clamped at startup) and `int g_msaa_max = 0;` (`GL_MAX_SAMPLES`, queried at startup).
  - [x] **Replace the MSAA checkbox** [viewer_window.cpp:664-678](../../src/viewer_window.cpp#L664-L678) with a 4-way selector, mirroring the **Shadow** RadioButton rhythm [viewer_window.cpp:491-501](../../src/viewer_window.cpp#L491-L501): a row of `ImGui::RadioButton("Off"/"2×"/"4×"/"8×", &g_msaa_level, 0/2/4/8)` (use `ImGui::SameLine()` between them; a label `ImGui::TextUnformatted("MSAA");` above, like Shadow). **Clamp to `g_msaa_max`:** an option whose sample count `> g_msaa_max` is shown disabled (`ImGui::BeginDisabled()/EndDisabled()`) — Off and any level `<= g_msaa_max` stay enabled. On a change, call `g_renderer.SetMsaaSamples(g_msaa_level);`.
  - [x] Leave the **Normal maps** and **FPS** checkboxes in the Performance section exactly as 6.5.5 shipped them [viewer_window.cpp:661-662, 680](../../src/viewer_window.cpp#L661).

- [x] **Task 5 — Revert the 6.5.5 window multisample bootstrap (`viewer_window.cpp`) (AC3).**
  - [x] **Delete** `ChooseMultisamplePixelFormat` (helper + its WGL constants/typedef) [viewer_window.cpp:242-323](../../src/viewer_window.cpp#L242-L323): the `WGL_*_ARB` `#define` block [viewer_window.cpp:246-257](../../src/viewer_window.cpp#L246), the `PFNWGLCHOOSEPIXELFORMATARBPROC` typedef [viewer_window.cpp:259-261](../../src/viewer_window.cpp#L259), and the function body.
  - [x] **`CreateGLContextFor`:** remove the multisample-format-first block [viewer_window.cpp:330-351](../../src/viewer_window.cpp#L330-L351); the window now takes **only** the legacy single-sample `ChoosePixelFormat`/`SetPixelFormat` path [viewer_window.cpp:355-377](../../src/viewer_window.cpp#L355-L377) (it stays verbatim — depth 24 / stencil 8 / double-buffer). Update the comment to say MSAA now lives in the offscreen FBO, not the window pixel format.
  - [x] **`StartRendering`:** replace the 6.5.5 `GL_SAMPLES`/`GL_SAMPLE_BUFFERS` availability query [viewer_window.cpp:752-763](../../src/viewer_window.cpp#L752-L763) with a one-time `glGetIntegerv(GL_MAX_SAMPLES, &g_msaa_max);` (context is current here). Then **seed + clamp** the level like `g_floor_visible` is seeded [viewer_window.cpp:601-614](../../src/viewer_window.cpp#L601-L614): clamp `g_msaa_level` to `g_msaa_max` (e.g. if `g_msaa_max < g_msaa_level`, drop to the largest valid level, or 0), then `g_renderer.SetMsaaSamples(g_msaa_level);` so UI/renderer agree from frame 1. Keep/repurpose or remove the local `GL_SAMPLES`/`GL_SAMPLE_BUFFERS` defines [viewer_window.cpp:41-47](../../src/viewer_window.cpp#L41) — they are no longer needed (remove to keep scope honest).

- [x] **Task 6 — Scope audit, gate §9, docs (AC6 + AR19/AR20).**
  - [x] `git diff --stat -- src/` shows **only**: `src/renderer.cpp`, `src/renderer.h`, `src/viewer_window.cpp`, `src/gl_loader.h`, `src/gpu_resources.h`. **No** `scene.h`/`CMakeLists.txt`/`plugin_main`/`reaper_api`/`pcm_source`/`asset_loader`/`overlay_icons.h`. Run `git diff -- src/ | grep -E 'rec->Register|REAPERAPI_WANT_'` → must be **empty** (AR15).
  - [x] **AR20 Spec Change Log** entry in [architecture.md](../planning-artifacts/architecture.md) (newest-on-top, the established format): **Trigger** (FR52 selectable MSAA via offscreen resolve, Story 6.5.6 — 6.5.5's fixed-format on/off couldn't change level live and `glDisable(GL_MULTISAMPLE)` was driver-ignored) / **Decision** (offscreen multisample **colour + depth** renderbuffer FBO, `glBlitFramebuffer`-resolved to FB0; window reverts to single-sample; level clamped to `GL_MAX_SAMPLES`; realises and **generalises** the long-intended 4× MSAA at architecture.md:391, now selectable) / **KEEP** (a real deviation from the Spike-0 "no offscreen **colour** pass" rule — **sibling to the 6.5.4 transient depth FBO**; AR15 register-symmetry, D2 zero-alloc per-frame [realloc only on level/resize cold path], AR17 non-fatal fallback to Off, single `SetPixelFormat` per HDC / no window recreation all hold).
  - [x] Append gate **§9** to [docs/PHASE4.5_VALIDATOR_GATE.md](../../docs/PHASE4.5_VALIDATOR_GATE.md) in the §7/§8 shape: intro + `> Scope note` (replaces 6.5.5's MSAA on/off; Normal-maps/FPS/load-failure unchanged; levels clamp to GPU max) + `Suggested fixtures:` + a **click-based** check table + `**Result:** … — PENDING`. **Never pre-mark PASS** (AR19).
  - [x] Update [deferred-work.md](deferred-work.md): annotate the 6.5.5 MSAA DONE entry [deferred-work.md:18](deferred-work.md#L18) to note the MSAA portion is **superseded by Story 6.5.6** (selectable levels via offscreen resolve); add a short 6.5.6 line if the tracker convention wants one. The Normal-maps/FPS/load-failure DONE entries stay.
  - [x] Build clean at `/W3 /permissive-` (NFR-R5). Linux can't build the `_WIN32` viewer/renderer/ImGui units or open Reaper → self-review (the offscreen-resolve is textbook GL 3.0; the cold-path realloc mirrors the proven 6.5.4 `AllocShadowMap`; the selector mirrors the proven 6.5.4 Shadow RadioButton); judged at Antho's in-Reaper Windows gate §9 (AR19).

## Dev Notes

### ⚠️ The UI is Dear ImGui (the epic's "Win32-child / GL-overlay" language is stale)

Since Story 6.5.3 the tool menu is **vendored Dear ImGui** (frameless hamburger; Antho: "C'est parfait"). The MSAA selector is **ImGui `RadioButton`s** in [DrawToolUi](../../src/viewer_window.cpp#L548), inside the Performance `Section` 6.5.5 added — **not** a Win32 child, **not** ReaImGui (a separate extension, post-MVP). Mirror the 6.5.4 **Shadow** selector exactly. [[project_imgui_ui]]

### Why offscreen-resolve, and why 6.5.5's approach failed

6.5.5 baked a multisample **window pixel format** at context creation and toggled `glEnable/glDisable(GL_MULTISAMPLE)` per frame. Two hard facts killed it: (1) `SetPixelFormat` is **one-shot per HDC** and the docked window/context is **never recreated** — so the sample count is **frozen at creation** and the level can't change live; (2) `glDisable(GL_MULTISAMPLE)` on the **default** framebuffer is honoured **inconsistently** across drivers — on Antho's it did nothing, hence "no visible difference". The games-standard fix is to render into an **offscreen multisample colour buffer** whose sample count lives in a **renderbuffer you can reallocate at runtime**, then **blit-resolve** it to the window. "Off" just renders straight to the window (today's path). [Source: sprint-change-proposal §1]

### The offscreen-resolve render flow (the heart of this story)

Per frame, `RenderFrame(anim, loop, width, height)`:

1. **Reconcile (cold path):** if the requested level (`msaa_samples_`) or the size differs from what's allocated, (re)allocate the MS targets — or free them at Off. This is the **only** allocation, and it happens off the hot path (D2). A resize naturally triggers it because `width`/`height` change (no extra WM_SIZE wiring needed).
2. `scene_fbo = (level > 0 && msaa_fbo_ valid) ? msaa_fbo_ : 0;`
3. `glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);` → set viewport → clear → **shadow depth pass** (own FBO, restores to **`scene_fbo`**, see below) → **floor pass** → **visible mesh pass**. All land in `scene_fbo`.
4. **If `scene_fbo != 0`:** `glBlitFramebuffer` the colour from `msaa_fbo_` (READ) to framebuffer 0 (DRAW), `GL_COLOR_BUFFER_BIT`, `GL_NEAREST` (src/dst are the same size → a straight multisample resolve). Then bind framebuffer 0.
5. Return with **framebuffer 0 bound**. `DrawToolUi` then draws ImGui onto the window over the resolved scene, and `SwapBuffers(g_hdc)` presents it. [Source: the RenderFrame body [renderer.cpp:578-745](../../src/renderer.cpp#L578); ImGui draws after RenderFrame, before SwapBuffers [viewer_window.cpp:449-455](../../src/viewer_window.cpp#L449)]

ImGui must **always** draw to the window, never into the MS buffer — that's why the resolve happens **inside** `RenderFrame` (before it returns) and `DrawToolUi` runs after. Do not move the ImGui draw into the MS pass.

### The shadow-pass restore is THE regression risk (AC5 — do not miss this)

The 6.5.4 shadow depth pass binds its own depth FBO, renders, then **unconditionally restores framebuffer 0** [renderer.cpp:715](../../src/renderer.cpp#L715). With offscreen MSAA the visible scene target is **no longer FB0** — it's `msaa_fbo_`. If the restore stays `…, 0)`, the floor + mesh draw into the **window** while the shadow expected them in the MS buffer, and the blit then **overwrites the window with an empty MS buffer** → a black/garbage viewport. Change that one line to restore **`scene_fbo`**. The shadow depth FBO bind, its `glViewport(0,0,shadow_map_size_,shadow_map_size_)`, and the `glCullFace` save/restore are otherwise unchanged. This is the single most likely thing to break — verify it explicitly. [Source: sprint-change-proposal §2]

### Colour format — match the window, do NOT use sRGB

The mesh shader **encodes linear→sRGB itself on the final write** (Story 6.5.1 — **not** `GL_FRAMEBUFFER_SRGB`) [renderer.cpp:155](../../src/renderer.cpp#L155), writing sRGB-encoded bytes into a plain `GL_RGBA8` window. The offscreen MS colour renderbuffer must therefore be **`GL_RGBA8`** (NOT `GL_SRGB8_ALPHA8`): the shader's output is already display-encoded, the window is plain RGBA8, and a `GL_RGBA8`→`GL_RGBA8` blit resolves byte-for-byte with **no colour shift** — identical to how a multisample default framebuffer resolves today. Using an sRGB renderbuffer here would double-encode and wash the image out. Depth renderbuffer = `GL_DEPTH_COMPONENT24` (matches the scene depth precision the legacy pixel format requested). [Source: renderer.cpp:155 sRGB-in-shader note; gl_loader.h:48 GL_SRGB8_ALPHA8 comment]

### Sample-count clamping (AC1)

Query `glGetIntegerv(GL_MAX_SAMPLES, &g_msaa_max)` once at startup (context current). The selector offers Off/2×/4×/8×; an option whose count exceeds `g_msaa_max` is **disabled** (a 2015-era iGPU often caps at 8, some at 4). Also clamp the **requested** value when allocating — `glRenderbufferStorageMultisample` with `samples > GL_MAX_SAMPLES` raises `GL_INVALID_VALUE`, so never pass an unclamped level. Default 4× is clamped down at startup if the GPU offers less. [Source: epics.md:708 "clamped to the GPU's maximum sample count"]

### Cold-path realloc mirrors the proven 6.5.4 shadow map

`AllocMsaaTargets` is structurally the same as `AllocShadowMap` [renderer.cpp:438-460](../../src/renderer.cpp#L438): gen into RAII handles via `.addr()`, configure, attach, `glCheckFramebufferStatus`, **restore the default binds before returning**, move-assign into the members on success, return `false` on any GL failure so the caller falls back to Off (AR17). The early-out `if (size unchanged) return true;` guard at [renderer.cpp:475](../../src/renderer.cpp#L475) is the precedent for the per-frame reconcile staying allocation-free. Reuse that discipline.

### Non-fatal everything (AR17/AR18)

Any MS FBO/renderbuffer allocation failure → free the partial targets, set the level to Off for the session, keep rendering to the window. Context creation **never** fails over MSAA (the window is single-sample now — MSAA can't block it). No exception escapes `WindowProc` (AR18). Console is silent by default (6.5.2) — any diagnostic is the wired-but-silent `LogWarn`/`LogInfo` funnel; **no** `printf`/`cout`/`OutputDebugString`. [Source: AR17 epics.md:165; console_log silent funnel [console_log.cpp:14-22](../../src/console_log.cpp#L14)]

### Regression guardrails

- **Off path is bit-identical to today + 6.5.5-minus-MSAA.** When `g_msaa_level == 0`, `scene_fbo == 0`, no MS targets exist, no blit runs — the renderer draws straight to the window exactly as before. This is the safe fallback for every failure too.
- **Mesh / skinning / material / floor / shadow render correctly at every level** — they draw to whatever FBO is bound; binding `scene_fbo` once at the top + the AC5 shadow-restore fix is all that's needed. Epic 2/3/4 + 6.5.1-6.5.5 rendering is otherwise unchanged.
- **Transport/playhead untouched** — the level is pure render state (AC6). `pcm_source_anim.cpp` byte-for-byte unchanged; session-only (no D9).
- **D2 zero-alloc per-frame** — MS targets (re)allocated only on level/resize cold path; the reconcile is a few int compares per frame.
- **Hide/show + reopen + resize:** all state is session-only; a resize re-allocates the MS buffer at the new size via the reconcile (the docked window is **not** recreated — the long-standing reason MSAA had to move off the pixel format). Docked-close sends us nothing — don't destroy on hide; the timer parks while `!IsWindowVisible` [viewer_window.cpp:480-493](../../src/viewer_window.cpp#L480). [[project_reaper_docked_close]]
- **Boundary (AR15):** only the five files in Task 6's audit; no `rec->Register`/`WANT_*`/CMake/plugin_main/reaper_api/pcm_source/asset_loader/scene.h.

### Latest-tech note (offscreen-resolve is stable, universally-supported core GL)

`glRenderbufferStorageMultisample`, `glBlitFramebuffer`, and `glFramebufferRenderbuffer` are **core since OpenGL 3.0 (2008)** (and `GL_ARB_framebuffer_object` before that) — there is no version risk on any GPU/driver this tool runs on; the framebuffer-object entry points already resolve in this codebase (6.5.4). No new library, no version pin, no breaking-change surface. This is the same family ReaImGui/sokol use under the hood; we call it directly via the existing `gl_loader.h` resolver. No web research warranted.

### Validation = Antho's in-Reaper Windows gate (AR19)

Linux compiles the non-`_WIN32` TUs but can't build the `_WIN32` viewer/renderer/ImGui units or open Reaper, so this is **self-reviewed** and judged **in-Reaper on Windows**. Author gate **§9 PENDING** — never pre-mark PASS. Validator hooks are **click-based** (double-click `build.bat`, click the hamburger → Performance → MSAA Off/2×/4×/8×, watch silhouette smoothness + FPS). [[feedback_clickable_test_methods]]

Suggested §9 checks (click-based): (1) the Performance MSAA control is a **4-level selector** Off/2×/4×/8× (options above the GPU max shown disabled); (2) stepping Off→2→4→8 **visibly and progressively smooths** jagged silhouettes, **live**, no flicker/crash; (3) **Off** looks like the pre-6.5.6 aliased image (and is the cheapest); (4) the **cast shadow + floor** (6.5.4) still render correctly at every level — no black/garbage viewport (the shadow-restore regression); (5) **Normal maps** + **FPS** (6.5.5) still work unchanged; (6) ≥60 fps holds at Off/2× (NFR-P1); (7) orbit/zoom/pan + Recenter + Light still work; resize the panel — the image stays correct at every level (cold-path realloc); (8) close→reopen returns to the default level, no ghost/leak, console silent (6.5.2).

### Project Structure Notes

- Flat `src/`. This story: `src/renderer.cpp` (AllocMsaaTargets + RenderFrame reconcile/bind/resolve + shadow-restore fix + remove GL_MULTISAMPLE toggle + Shutdown teardown), `src/renderer.h` (`SetMsaaSamples`/`msaa_samples_` + MS-target members + AllocMsaaTargets decl, replacing `SetMsaaEnabled`/`msaa_on_`), `src/viewer_window.cpp` (4-level selector + `g_msaa_level`/`g_msaa_max`, replacing the 6.5.5 checkbox + `g_msaa_on`/`g_msaa_available`/`g_msaa_samples`; revert the WGL bootstrap; `GL_MAX_SAMPLES` query in StartRendering), `src/gl_loader.h` (renderbuffer + blit rows + enums; remove GL_MULTISAMPLE), `src/gpu_resources.h` (`GpuRenderbuffer` alias). Docs: gate §9, AR20 Spec Change Log, deferred-work, sprint-status.
- Naming: members `snake_case_` (`msaa_samples_`, `msaa_color_rb_`); globals `g_` (`g_msaa_level`, `g_msaa_max`); methods PascalCase (`SetMsaaSamples`, `AllocMsaaTargets`). Namespace `rav`; SPDX MIT header; WHY-only comments.

### References

- [Source: epics.md#Story-6.5.6 (lines 698-713)] — the story ACs (multi-level selector, offscreen resolve, cold-path realloc, non-fatal).
- [Source: sprint-change-proposal-2026-06-28-msaa.md] — issue, root cause, impact, file list, the "shadow pass restores to the active scene target" requirement.
- [Source: epics.md:160-171] — AR15 (:163), AR17 (:165), AR18 (:166), AR19 (:170), AR20 (:171). [Source: architecture.md:380-396] — direct-render "no offscreen FBO/MSAA-resolve" amendment + the MSAA-4× intent row (:391) this generalises; [Source: architecture.md:211-235] — D2 zero-alloc.
- [Source: 6-5-5-render-quality-toggles-and-on-canvas-fps.md] — the Performance section + MSAA checkbox/globals/bootstrap this story replaces; the Normal-maps/FPS/load-failure deliverables it leaves intact; the gate/AR20/deferred-work rhythm.
- [Source: 6-5-4-floor-tool-solid-grid-show-hide.md] — the transient-FBO precedent, `AllocShadowMap`/`SetShadowQuality` cold-path realloc, the Shadow 4-way RadioButton to mirror, the shadow-pass restore being fixed here.
- [Source: src/renderer.cpp:438-487] — `AllocShadowMap`/`SetShadowQuality` (the realloc pattern); :578-745 `RenderFrame` (bind/clear/shadow/floor/mesh — the body to wrap); :585-591 the GL_MULTISAMPLE toggle to remove; :715 the shadow restore to fix; :155 the sRGB-in-shader write; :790-793 Shutdown teardown.
- [Source: src/renderer.h:113-124] — `SetFloorVisible`/`SetMsaaEnabled` inline-setter precedent; :217 `msaa_on_`; :228-238 the shadow members to add the MS targets beside.
- [Source: src/viewer_window.cpp:41-47] GL_SAMPLES local enums; :242-351 the WGL MSAA bootstrap to revert; :355-377 the legacy pixel-format path that stays; :449-455 RenderFrame→ImGui→SwapBuffers order; :491-501 the Shadow RadioButton to mirror; :601-614 the `g_floor_visible` seed pattern; :655-683 the Performance section; :752-763 the GL_SAMPLES availability query to replace with GL_MAX_SAMPLES.
- [Source: src/gl_loader.h:58-86, 122-170] — the FBO enum/X-macro/`#define` blocks to extend; remove GL_MULTISAMPLE (:80-86).
- [Source: src/gpu_resources.h:45-55] — the deleter/alias pattern for `GpuRenderbuffer`.

## Dev Agent Record

### Agent Model Used

Claude Opus 4.8 (claude-opus-4-8[1m]) — BMAD dev-story workflow, 2026-06-29.

### Debug Log References

- Scope audit: `git diff --stat -- src/` → only the 5 permitted files (`renderer.{h,cpp}`, `viewer_window.cpp`, `gl_loader.h`, `gpu_resources.h`); `git diff -- src/ | grep -E 'rec->Register|REAPERAPI_WANT_'` → empty (AR15 clean).
- Dangling-symbol sweep after the 6.5.5 revert: `g_msaa_on` / `g_msaa_available` / `g_msaa_samples` / `GL_MULTISAMPLE` / `ChooseMultisamplePixelFormat` / `msaa_on_` / `SetMsaaEnabled` / `GL_SAMPLES` / `GL_SAMPLE_BUFFERS` / `WGL_*` → no code references remain (only one intentional mention inside the new `CreateGLContextFor` comment explaining the revert).
- gl_loader.h consistency: the 6 new X-macro rows match their `#define` routing lines exactly (verified by name cross-grep); `glBlitFramebuffer` is the last row (no trailing `\`).
- Build: Linux dev box cannot build the `_WIN32` viewer/renderer/ImGui TUs or open Reaper (all changed code is inside `#ifdef _WIN32`). Self-reviewed per AR19; preprocessor `#if/#endif` balance confirmed per file; judged at Antho's in-Reaper Windows gate §9.

### Completion Notes List

- **AC1 — 4-level selector clamped to GPU max.** The 6.5.5 MSAA checkbox is replaced by an Off / 2× / 4× / 8× `ImGui::RadioButton` row in the Performance section, mirroring the 6.5.4 Shadow selector. `GL_MAX_SAMPLES` is queried once at startup (`g_msaa_max`); an option above it is shown disabled via `BeginDisabled`/`EndDisabled`. Labels are ASCII `2x`/`4x`/`8x` (the build has no `/utf-8`; a raw `×` in a string literal trips MSVC C4566 under `/W3` — same convention as the 6.5.3 labels). The "Off" radio is id-tagged `Off##msaa` so it doesn't collide with the Shadow section's "Off".
- **AC2/AC3 — live, offscreen resolve.** MSAA is delivered by an offscreen multisample colour FBO (`GL_RGBA8` + `GL_DEPTH_COMPONENT24` renderbuffers) blit-resolved to FB0; the level changes live with no window/context recreation. The 6.5.5 per-frame `glEnable/glDisable(GL_MULTISAMPLE)` and the window multisample bootstrap (`ChooseMultisamplePixelFormat` + the MS branch of `CreateGLContextFor`) are removed — the window is single-sample now.
- **AC4 — cold-path realloc, Off has no offscreen cost.** `AllocMsaaTargets` (mirrors `AllocShadowMap`) runs only on a level/size change; the per-frame reconcile is int compares (D2). At Off the targets are freed and the scene renders straight to FB0.
- **AC5 — shadow-restore regression fixed.** The 6.5.4 shadow depth pass now restores to `scene_fbo` (the MS FBO when on, FB0 when Off) instead of unconditionally FB0 — without this the floor/mesh would draw to the window and the resolve would overwrite it blank. Verified explicitly (`renderer.cpp` shadow pass).
- **AC6 — boundary discipline.** Confined to the 5 declared files; no `scene.h`/`CMakeLists.txt`/`plugin_main`/`reaper_api`/`pcm_source`/`asset_loader`/`overlay_icons.h` change; no `rec->Register`, no new `REAPERAPI_WANT_*`. Session-only (no D9; `pcm_source_anim.cpp` untouched). `SetMsaaSamples(int)` replaces `SetMsaaEnabled(bool)`.
- **AR17 non-fatal.** Any FBO/renderbuffer allocation failure frees the partial targets and falls back to a working no-MSAA (Off) view; the reconcile records the requested config on failure so it does **not** re-attempt the failing alloc every frame (a deliberate refinement of the spec's `msaa_alloc_samples_ = 0` example, which would have retried each frame). A later level change or resize gives it a fresh attempt. Context creation can never fail over MSAA now (the window is single-sample).
- **Empty-panel correctness (not in the spec, added defensively).** When no asset is loaded with MSAA on, the cleared MS buffer is still blit-resolved to the window before the early return, so an empty panel is never left stale/garbage.
- **Colour format.** Offscreen colour renderbuffer is `GL_RGBA8` (NOT sRGB): the mesh shader already encodes linear→sRGB on its final write (6.5.1), so an `RGBA8→RGBA8` resolve is byte-for-byte with no colour shift.
- Docs: AR20 Spec Change Log entry (architecture.md, newest-on-top), gate §9 (PENDING — never pre-marked PASS, AR19), deferred-work.md (6.5.5 MSAA entry annotated as superseded + a new 6.5.6 deferral block).

### File List

- `src/gl_loader.h` — 6 append-only renderbuffer/blit X-macro rows + `#define` routing; new guarded enums (`GL_RENDERBUFFER`, `GL_COLOR_ATTACHMENT0`, `GL_READ_FRAMEBUFFER`, `GL_DRAW_FRAMEBUFFER`, `GL_MAX_SAMPLES`); removed the now-unused `GL_MULTISAMPLE` define.
- `src/gpu_resources.h` — `RenderbufferDeleter` + `GpuRenderbuffer` RAII alias.
- `src/renderer.h` — `SetMsaaSamples(int)`/`msaa_samples_` replace `SetMsaaEnabled(bool)`/`msaa_on_`; MS-target members + alloc cache; `AllocMsaaTargets` decl; class-header comment notes the offscreen MS colour FBO + blit-resolve deviation.
- `src/renderer.cpp` — `AllocMsaaTargets`; RenderFrame MSAA reconcile + scene-target bind + blit-resolve; shadow-pass restore to `scene_fbo`; removed the per-frame `GL_MULTISAMPLE` toggle; Shutdown frees the MS targets.
- `src/viewer_window.cpp` — 4-level MSAA selector + `g_msaa_level`/`g_msaa_max` (replacing the 6.5.5 checkbox + `g_msaa_on`/`g_msaa_available`/`g_msaa_samples`); reverted the WGL multisample bootstrap (window single-sample); `GL_MAX_SAMPLES` query + seed/clamp in StartRendering; removed the `GL_SAMPLES`/`GL_SAMPLE_BUFFERS` local enums.
- `_bmad-output/planning-artifacts/architecture.md` — AR20 Spec Change Log entry (2026-06-29).
- `docs/PHASE4.5_VALIDATOR_GATE.md` — gate §9 (PENDING).
- `_bmad-output/implementation-artifacts/deferred-work.md` — 6.5.5 MSAA entry annotated superseded + 6.5.6 deferral block.
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — 6-5-6 status ready-for-dev → in-progress → review.

### Change Log

| Date | Change |
|---|---|
| 2026-06-29 | Implemented Story 6.5.6 — selectable MSAA (Off/2×/4×/8×) via an offscreen multisample colour FBO + blit-resolve; reverted the 6.5.5 window multisample bootstrap; fixed the 6.5.4 shadow-pass restore to the active scene target. All 6 tasks complete; Status → review; gate §9 PENDING (Antho in-Reaper Windows, AR19). |
