# Sprint Change Proposal — Epic 6.5: Viewport visual fidelity & on-canvas tools

- **Date:** 2026-06-27
- **Author:** Antho (validator) + Claude (dev)
- **Workflow:** Correct Course (BMAD)
- **Mode:** Batch
- **Trigger:** Pre-ship visual-quality gap + new viewport-tooling requests, raised by Antho between Epic 6 (done) and Epic 7 (ship)
- **Scope classification:** **Moderate** — new epic inserted into the sequence, two artifact-invariant amendments (AR16, PRD Growth Features), no rollback.

---

## Section 1 — Issue Summary

While reviewing the rendered output of downloaded models (Mixamo reference vs. our in-Reaper render), four concerns surfaced, all to be resolved **before** the ReaPack ship (Epic 7):

1. **Render fidelity is wrong.** The same model that looks correct in Mixamo renders in our viewer **too dark, washed-out, with a plastic/metallic sheen and missing surface detail/colour.** Side-by-side screenshots provided by Antho confirm the gap (Mixamo: warm, detailed, readable skin; ReaAnimViewer: dark, desaturated, metallic).
2. **Console logging is noise.** Antho wants **all** console logs removed.
3. **No on-canvas tooling.** Antho wants a **vertical icon strip in the top-right corner** of the viewport — a menu surface for future tools. First tool: a **light** icon that, on click, expands options to choose the **light colour** and its **position around the origin**.
4. **No floor control.** A sidebar icon to **show/hide a solid + grid floor.**

During scoping, Antho extended (3)/(4): the menu must also host **render-quality toggles** (disable costly render elements on weak PCs) and a **FPS readout** (new icon; FPS shown top-right) — since the FPS figure is today logged to console and (2) removes that channel.

### Evidence (grounded in code)

| Symptom | Root cause (file:line) |
|---|---|
| Washed-out / wrong colours | No sRGB pipeline — base-colour texture uploaded as linear `GL_RGBA8` ([asset_loader.cpp:483](../../src/asset_loader.cpp#L483)) and sampled as linear with no gamma decode/encode ([renderer.cpp:91](../../src/renderer.cpp#L91)); no `GL_FRAMEBUFFER_SRGB`. |
| Too dark | Single hardcoded directional light `normalize(vec3(0.4,0.9,0.5))` + fixed ambient `0.15` ([renderer.cpp:85,94](../../src/renderer.cpp#L85)) → back/fill faces crush to near-black. |
| Plastic / metallic | Specular tinted from glTF *metallic* factor `0.04 + (base-0.04)*metallic` with default shininess 32 ([asset_loader.cpp:425,453-460](../../src/asset_loader.cpp#L453)) → broad highlights on skin (a dielectric). |
| Missing surface detail | Normal maps not sampled (only base colour); no tangents in vertex data. Per PRD this was a **Growth Feature**. |
| Console noise | 43 log call-sites, all funnelling through one helper `Emit()` ([console_log.cpp:14-22](../../src/console_log.cpp#L14-L22)) — single clean removal point. |
| FPS today in console | `LogInfo("%.1f fps …")` once/sec ([viewer_window.cpp:237](../../src/viewer_window.cpp#L237)) — orphaned when logs are removed → motivates on-canvas FPS. |
| No GL UI framework | Viewport is pure native OpenGL; the only existing widget is a Win32 child **"Reset View"** button ([viewer_window.cpp:349](../../src/viewer_window.cpp#L349)). ReaImGui still deferred. → sidebar extends the Win32-child pattern, no ReaImGui pulled in early. |

---

## Section 2 — Impact Analysis

### Epic impact
- **Epic 6 (done):** unaffected — no rollback. Save/recall is orthogonal.
- **New Epic 6.5 inserted** between Epic 6 and Epic 7. Numbered `6.5` to avoid renumbering the ship (Epic 7) and post-release (Epic 8) epics and their sprint-status keys / cross-references.
- **Epic 7 (ship):** unchanged in content; simply **resequenced after 6.5**. The MVP ships with corrected fidelity + the tool sidebar.
- **Epic 8 (post-release):** unaffected. (Note: normal maps were partly an Epic-8/Growth item; pulling them forward is an explicit, authorised scope move — see PRD impact.)

### Artifact conflicts (the two that matter)
1. **AR16 / D7 — "console-only diagnostics" invariant** (architecture.md:93, :290, :878). Removing *all* logs contradicts the stated invariant that `ShowConsoleMsg` is the canonical user-feedback channel. **Resolution:** amend AR16 → *"diagnostics are silent by default; the few genuinely user-facing signals (FPS, and a load-failure indication) are surfaced on-canvas, not in the console."* The central funnel `Emit()` is **kept but made a no-op by default** (compile-time switch) so a debug build can still light it up — we delete noise, not the ability to diagnose.
2. **PRD Growth Features — "normal maps / richer PBR = post-MVP"** (prd.md:151, architecture.md:183,1107). Choosing "la totale" (incl. normal maps) **pulls a Growth Feature into the MVP.** **Resolution:** PRD/architecture amended to move sRGB-correct shading + normal maps from Growth → MVP (new FR48). Authorised by Antho 2026-06-27.

### Architecture / data-model touch-points
- `SceneMaterial { GpuImage baseColor; vec3 specularColor; float shininess; }` → add `GpuImage normalMap;` + per-vertex **tangents** (architecture.md:195).
- Texture-format table "RGBA8 sufficient" → base colour uploaded **`GL_SRGB8_ALPHA8`**; output gamma via `GL_FRAMEBUFFER_SRGB` (architecture.md:389).
- New shader uniforms: `u_lightColor`, `u_lightDir` (or position), `u_ambient` — drive both the fidelity fix (6.5.1) and the light tool (6.5.3).
- New UI component: **viewport tool sidebar** (top-right) + flyout panels + native colour chooser — documented as the on-canvas UI model (Win32 child controls / GL overlay, **no ReaImGui**).

### NFR alignment (no conflict — additive)
- **NFR-P1 ≥60 fps** is *served* by the render-quality toggles (graceful degradation knob) and made *observable* by the on-canvas FPS readout.

---

## Section 3 — Recommended Path Forward

**Selected: Option 1 — Direct Adjustment (new epic, no rollback).** A new Epic 6.5 of **5 stories** added before Epic 7.

- **Effort:** Medium. **Risk:** Low–Medium (normal maps + tangents add pipeline surface; sRGB/lighting are localized shader changes).
- **Rollback (Option 2):** Not applicable — nothing completed needs reverting.
- **MVP review (Option 3):** Triggered only to the extent of the two authorised amendments (AR16, normal-maps-into-MVP); the MVP goal is *strengthened*, not reduced.

**Why a new epic over one big story:** the four+ concerns share infrastructure (the sidebar serves light *and* floor *and* perf *and* FPS; the light tool depends on the lighting refactor) but have **independent in-Reaper validation gates**. Five stories keep a texture bug from being entangled with a UI bug at the validator gate (Antho's preferred per-gate closure).

---

## Section 4 — Detailed Change Proposals

### 4.A New Epic (append to `epics.md`, between Epic 6 and Epic 7)

> ## Epic 6.5: Viewport visual fidelity & on-canvas tools
>
> Bring the rendered output up to source-DCC fidelity (Mixamo parity) and add an on-canvas tool sidebar, **before** shipping. Fixes the washed-out/dark/metallic render (sRGB pipeline, balanced lighting, dielectric-correct specular, normal maps), removes console-log noise, and introduces a top-right vertical icon strip hosting: a **light** tool (colour + position around origin), a **floor** tool (solid + grid show/hide), **render-quality** toggles (degrade gracefully on weak PCs), and an on-canvas **FPS** readout. *(Phase 4.5, pre-ship polish — Antho 2026-06-27. Honors AR15/AR17/AR18; amends AR16 console-only and pulls normal maps from Growth → MVP per FR48. No ReaImGui — the sidebar extends the existing Win32-child widget pattern.)*
> **FRs covered:** FR48 (render fidelity), FR49 (tool sidebar), FR50 (light tool), FR51 (floor tool), FR52 (render-quality toggles), FR53 (on-canvas FPS). Amends FR16, AR16.

#### Story 6.5.1 — Source-fidelity rendering (sRGB + lighting + dielectric specular + normal maps)
> As a sound designer, I want models to look like they do where I downloaded them, so that what I review in Reaper matches the source.
>
> **AC:**
> **Given** a textured glTF/GLB (e.g. a Mixamo character) **When** it renders **Then** colours match the source DCC within reasonable tolerance: base-colour texture is treated as **sRGB** (`GL_SRGB8_ALPHA8`) and output is gamma-correct (`GL_FRAMEBUFFER_SRGB`)
> **And** lighting no longer crushes to black — a balanced ambient/fill term keeps unlit faces readable; light colour/direction are driven by uniforms (`u_lightColor`, `u_lightDir`, `u_ambient`) consumable by the light tool (6.5.3)
> **And** specular no longer reads metallic on dielectric surfaces (skin) — specular strength/shininess tuned so a non-metal material is matte by default
> **And** **normal maps**, when present in the asset, are sampled (tangents added to vertex data; `SceneMaterial` gains `normalMap`) restoring surface detail; assets without a normal map render unchanged
> **And** no regression to skinned/static rendering (Epics 2–3) and ≥60 fps holds at the NFR-P1 fixture.

#### Story 6.5.2 — Silence all console logging by default
> As a sound designer, I don't want console spam from the viewer.
>
> **AC:**
> **Given** normal operation **When** I load/play/save animations **Then** **no** `[RAV]` console output is produced (all 43 sites silenced via the single `Emit()` funnel)
> **And** the funnel is retained as a **no-op by default**, re-enablable in a debug build (does not delete the diagnostic capability — amends AR16: console-only → silent-by-default + on-canvas signals)
> **And** the FPS figure and any load-failure signal are **not** orphaned — they are rehomed on-canvas (6.5.5 / a minimal on-canvas indication).

#### Story 6.5.3 — Viewport tool sidebar + Light tool
> As a sound designer, I want a top-right tool menu, starting with a light control, so that I can adjust how the model is lit.
>
> **AC:**
> **Given** the viewport **When** it renders **Then** a **vertical icon strip** appears in the **top-right** corner, built to host multiple tools (extensible), using the existing Win32-child/GL-overlay pattern — **no ReaImGui**
> **And** clicking the **light** icon expands a flyout to choose the **light colour** (native colour picker) and the **light position around the origin** (e.g. azimuth/elevation), live-updating the `u_lightColor`/`u_lightDir` uniforms from 6.5.1
> **And** the existing "Reset View" control continues to work; sidebar interaction never blocks the host or leaks resources (AR18).

#### Story 6.5.4 — Floor tool (solid + grid show/hide)
> As a sound designer, I want to toggle the ground, so that I can frame the model how I like.
>
> **AC:**
> **Given** the sidebar **When** I click the **floor** icon **Then** a solid ground plane + grid toggles visible/hidden (state applies immediately)
> **And** the toggle has no effect on model rendering or transport, and survives nothing beyond the session unless trivially free to persist (persistence is **not** required here).

#### Story 6.5.5 — Render-quality toggles + on-canvas FPS readout
> As a sound designer on a weaker PC, I want to drop expensive render elements and see my FPS, so that playback stays smooth.
>
> **AC:**
> **Given** the sidebar **When** I open the **performance** tool **Then** I can toggle costly render elements (e.g. normal maps, MSAA, floor) off/on, with immediate effect
> **And** an **FPS** icon toggles an on-canvas FPS readout shown **top-right** (replacing the removed console FPS log)
> **And** toggling elements never crashes the host and is purely visual/perf (no transport or data impact).

### 4.B PRD edits (`prd.md`)
- **Growth Features (prd.md:151):** move *"Normal maps and richer material rendering"* → **MVP**, scoped to sRGB-correct shading + normal-map sampling (Blinn-Phong stays; not full PBR). Add FR48.
- **New FRs:** FR48 source-fidelity rendering; FR49 tool sidebar; FR50 light tool; FR51 floor tool; FR52 render-quality toggles; FR53 on-canvas FPS.
- **FR16:** annotate "+ sRGB-correct + optional normal map (FR48)".

### 4.C Architecture edits (`architecture.md`)
- **AR16 (cross-cutting concern #4, :93/:290/:878):** amend to *silent-by-default diagnostics + on-canvas FPS/error signal*; `log.*` funnel kept as compile-time-gated no-op.
- **`SceneMaterial` (:195):** add `GpuImage normalMap;` + tangents in vertex layout.
- **Texture format table (:389):** base colour → `GL_SRGB8_ALPHA8`; enable `GL_FRAMEBUFFER_SRGB`.
- **New section — On-canvas UI model:** viewport tool sidebar (top-right), flyouts, native colour chooser, render-quality flags; **no ReaImGui** (deferral intact). New shader uniforms `u_lightColor`/`u_lightDir`/`u_ambient`.
- **Growth Features (:183,:1107):** strike normal maps as the part promoted to MVP.

### 4.D Sprint-status edits (`sprint-status.yaml`)
- Insert `epic-6.5: backlog` between `epic-6` and `epic-7`, with stories `6.5.1`–`6.5.5` (`backlog`) and `epic-6.5-retrospective: optional`. Epic-7/Epic-8 keys unchanged.

---

## Section 5 — Implementation Handoff

- **Scope:** Moderate → **PO/Dev coordination.** Backlog reorg (new epic + 5 stories + 6 new FRs + 2 invariant amendments), then standard story-by-story dev.
- **Sequencing (hard dependency in bold):** **6.5.1 → 6.5.3**; 6.5.3 (sidebar infra) → 6.5.4, 6.5.5; 6.5.2 anytime but FPS rehome lands in 6.5.5. Recommended order: 6.5.1, 6.5.2, 6.5.3, 6.5.4, 6.5.5.
- **Validation:** each story closes on its own in-Reaper Windows gate (AR19), per Antho's per-gate closure rule. 6.5.1 gate = Mixamo side-by-side parity judged acceptable by Antho.
- **Next step after approval:** update `epics.md`, `prd.md`, `architecture.md`, `sprint-status.yaml` per Section 4, then `create-story 6.5.1`.

---

## Success criteria
- A Mixamo model renders close to its source appearance (Antho-judged), no console output in normal use, a working top-right tool sidebar with light + floor + render-quality + FPS tools, MVP still ships in Epic 7 with ≥60 fps held.
