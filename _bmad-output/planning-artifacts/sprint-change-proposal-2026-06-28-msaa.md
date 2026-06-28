# Sprint Change Proposal — Selectable MSAA quality levels (Story 6.5.6)

**Date:** 2026-06-28
**Author:** Claude (dev) with Antho (Demute)
**Scope classification:** **Moderate** — backlog insertion of one new story that supersedes the MSAA portion of an in-review story; no epic replan.
**Mode:** Batch.

---

## 1. Issue Summary

While validating Story 6.5.5 in-Reaper, Antho found the **MSAA on/off checkbox produced no visible difference**, and asked for a **games-style MSAA quality selector** (Off / 2× / 4× / 8×) instead of a single on/off.

Root cause of the limitation in 6.5.5's approach:

- 6.5.5 enables MSAA by baking a multisample **window pixel format** at context creation (`wglChoosePixelFormatARB`) and toggling `glEnable/glDisable(GL_MULTISAMPLE)` per frame.
- Under Win32 the pixel format's **sample count is fixed once** (`SetPixelFormat` is one-shot; the docked window/context is never recreated) — so the **level cannot change live**.
- `glDisable(GL_MULTISAMPLE)` on the default framebuffer is **honoured inconsistently** across drivers — on some it does nothing, which is the "no visible difference" Antho observed.

The correct, robust, games-standard way to get **selectable, live** MSAA levels is to render the scene into an **offscreen multisample colour buffer** and **resolve (blit) it to the window** — the sample count then lives in a renderbuffer that **can** be reallocated at runtime, and "Off" renders directly to the window.

## 2. Impact Analysis

- **Epic impact:** Epic 6.5 (pre-ship polish) gains **Story 6.5.6**, inserted after 6.5.5, before Epic 7. Epic stays open one more story; no structural replan.
- **Story impact:**
  - **6.5.5** stays as shipped (Status: review → its own gate §8). Its **Normal-maps toggle, FPS readout, and on-canvas load-failure** are unaffected.
  - **6.5.6** **replaces the MSAA portion** of 6.5.5: reverts the context-creation multisample bootstrap + the `glEnable/glDisable(GL_MULTISAMPLE)` per-frame toggle, and delivers the offscreen-resolve pipeline + level selector.
- **PRD:** FR52 amended (note: MSAA becomes a multi-level selector via offscreen resolve — Story 6.5.6). No new FR (enhancement of FR52).
- **Architecture:** A real deviation from the Spike-0 direct-render "no offscreen **colour** pass" rule — an **offscreen multisample colour pass + resolve**. AR20 Spec Change Log entry authored during dev-story 6.5.6 (sibling to the 6.5.4 transient **depth** FBO deviation; realises the long-intended 4× MSAA at architecture.md:391, now generalised to selectable levels). AR15 register-symmetry, D2 zero-alloc per-frame, AR17 non-fatal all KEEP.
- **Technical impact:** `renderer.{h,cpp}` (offscreen MS colour+depth renderbuffers, reallocated on level/resize cold path; bind → render → `glBlitFramebuffer` resolve to FB0; ImGui draws to FB0 after; the 6.5.4 shadow pass restores to the active scene target instead of FB0); `viewer_window.cpp` (revert MSAA window bootstrap; MSAA level UI = multi-level selector; clamp options to `GL_MAX_SAMPLES`); `gl_loader.h` (append renderbuffer + `glBlitFramebuffer` rows + enums); `gpu_resources.h` (`GpuRenderbuffer` RAII alias). **No** `rec->Register` / `REAPERAPI_WANT_*` / `CMakeLists` / `plugin_main` / `reaper_api` / `pcm_source` / `asset_loader` / `scene.h` change.

## 3. Recommended Approach

**Direct adjustment** — add Story 6.5.6 to Epic 6.5 (the chosen path from the options Antho was offered: "do it properly"). It is the correct long-term design, it gives Antho the selectable levels he wants, and it fixes the 6.5.5 "no visible difference". Risk is **moderate** (rewrites the visible render target; only testable in-Reaper on Windows; lands pre-ship), mitigated by: it is textbook GL, the FBO entry points already exist from 6.5.4, "Off" keeps today's direct path, and every failure path falls back non-fatally (AR17).

- **Effort:** ~1 focused dev-story (~120–180 LOC across the files above).
- **Timeline:** one extra story before Epic 7; Epic 6.5 closes after 6.5.6's gate.

## 4. Detailed Change Proposals

1. **`prd.md` FR52** — appended amendment note (MSAA → multi-level selector via offscreen resolve, Story 6.5.6). *(applied)*
2. **`epics.md`** — Epic 6.5 "FRs covered" line annotated; **Story 6.5.6** added after 6.5.5 with full acceptance criteria. *(applied)*
3. **`sprint-status.yaml`** — `6-5-6-selectable-msaa-quality-levels: backlog` inserted after 6-5-5, before `epic-6.5-retrospective`, with a full rationale comment. *(applied)*
4. **`architecture.md`** — AR20 Spec Change Log entry for the offscreen-colour-resolve deviation: authored during **dev-story 6.5.6** (per the AR20 convention), not here.

## 5. Implementation Handoff

- **Scope:** Moderate → **create-story** for `6-5-6-selectable-msaa-quality-levels` (context-rich spec), then **dev-story**, then Antho's in-Reaper Windows **gate §9** (AR19).
- **Success criteria:** the Performance section shows an **MSAA Off/2×/4×/8×** selector (clamped to GPU max); changing the level visibly and live changes silhouette smoothness with no flicker/crash; Off has no offscreen cost; ≥60 fps holds at lower levels (NFR-P1); no transport/persistence/host impact; 6.5.5's Normal-maps/FPS/load-failure unchanged.
- **6.5.5 disposition:** unchanged — it keeps its review status and gate §8 (its non-MSAA deliverables stand).
