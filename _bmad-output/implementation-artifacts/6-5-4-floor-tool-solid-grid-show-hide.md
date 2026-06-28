---
baseline_commit: 35a87a9ba8e57f51a6aa12067cf4af76b3f72b1f
---

# Story 6.5.4: Always-on floor + shadow-quality tool (Off / Low / Mid / High)

Status: done

<!-- Note: Validation is optional. Run validate-create-story for quality check before dev-story. -->

> **Scope revision (Antho, 2026-06-28):** the floor is **always on** — there is **no** show/hide toggle
> (we always want a ground). The sidebar control is instead a **shadows** tool (Antho's new
> `Icons/icon_shadow.svg`): the model casts a real shadow onto the floor, and clicking the shadow icon
> offers a **4-level quality selector — Off / Low / Mid / High** — so a user on a weaker PC can lower or
> disable shadows if the render struggles. This re-shapes FR51 (floor, now non-toggled) and pulls a
> slice of **FR52** (render-quality / graceful degradation) forward into this story; the remaining FR52
> levers (normal maps, MSAA) + FR53 FPS stay in 6.5.5. The sprint key keeps its original
> `…-floor-tool-solid-grid-show-hide` name for traceability.

## Story

As a sound designer,
I want a ground plane under the model with a real cast shadow whose quality I can dial down (or off),
so that the model is nicely grounded and I can keep playback smooth on a weaker machine.

## Acceptance Criteria

1. **Given** a model is loaded
   **When** it renders
   **Then** a **solid ground plane + grid is always shown** at the model's feet (its base, `aabbMin.y`), centred under the model and sized to it — no toggle, it is simply part of the scene (FR51, revised). The empty idle panel (no asset) renders exactly as today (no floor). [Source: epics.md#Story-6.5.4 / FR51; Antho scope revision 2026-06-28; AABB [scene.h:90-91](../../src/scene.h#L90-L91), frameRadius [camera.h:37,90](../../src/camera.h#L37)]

2. **Given** the viewport tool menu (the frameless Dear ImGui hamburger menu from Story 6.5.3)
   **When** I click the **shadows** control (Antho's `icon_shadow`)
   **Then** it offers a **4-way quality selector — Off / Low / Mid / High** — that takes effect **immediately** (next ~66 Hz frame) and reflects the current selection; the model casts a soft shadow onto the floor whose resolution/softness rises with the level, and **Off** removes the shadow entirely (and its per-frame cost). [Source: epics.md#Story-6.5.4 (FR51 sidebar), FR52 graceful-degradation intent; menu [viewer_window.cpp:385-451](../../src/viewer_window.cpp#L385-L451)]

3. **Given** any shadow quality
   **When** the model animates and the camera moves
   **Then** the shadow tracks the model and the light (it is rendered from the **current** `light_dir_`, so the 6.5.3 light-position pad moves the shadow too), it has **no effect on transport** (the playhead/scrub/clamp path is untouched), and the model's **own** rendering (colour/lighting/skinning) is unchanged except for the optional self-shadow term. [Source: light members [renderer.h:123-124](../../src/renderer.h#L123); transport [viewer_window.cpp:235-302](../../src/viewer_window.cpp#L235)]

4. **Given** weak-hardware operation
   **Then** **Off** and **Low** hold **≥60 fps** at the NFR-P1 fixture (Off adds **zero** cost — the depth pass is skipped; the floor still draws); Mid/High are quality-up options that may cost more by design (that is the point of the selector). Shadow-map textures/FBO are (re)allocated only on a **quality change** (cold path) — the per-frame path stays **allocation-free** (D2). [Source: NFR-P1 architecture.md, D2 zero-alloc]

5. **Given** the boundary discipline
   **Then** the change is confined to `renderer.{h,cpp}`, `viewer_window.cpp`, the **regenerated** `overlay_icons.h` (+ `Icons/icon_shadow.svg`, `tools/gen_icons.py`), and a **justified, append-only** addition of **FBO function rows to `gl_loader.h`** (shadow mapping needs GL 3.0 framebuffer entry points not yet in the table — precedented: Stories 6.5.1 and 3.3 each appended a GL-loader row; log it in AR20). **No** `scene.h` / `CMakeLists.txt` / `plugin_main` / `reaper_api` / `pcm_source` / `asset_loader` change; **no** `rec->Register`, **no** new `REAPERAPI_WANT_*` (the canonical AR15 register-symmetry invariant is intact). [Source: AR15 epics.md:163, AR20 epics.md:171; gl_loader precedent 6.5.1/3.3]

6. **Given** session-only state and clean teardown
   **Then** the shadow quality is **not** persisted (no `.rpp`/D9/`pcm_source` wiring — a fresh viewer opens at the default level), shadow-shader/FBO failures are **non-fatal** (`LogWarn` once, fall back to no-shadow, the viewport still runs — AR17), and all new GL objects (shadow FBO + depth texture + floor buffers) are RAII members freed in `Shutdown` while the context is current (NFR-R3 symmetric teardown). [Source: AR17 epics.md:165, AR18 epics.md:166, D9 not in scope]

### Out of scope (explicitly deferred — do NOT implement here)

- **Normal-map / MSAA quality toggles + on-canvas FPS readout** → Story 6.5.5 (FR52/FR53). This story brings only the **shadow** quality lever forward; do not add a normal-map switch, MSAA, or any FPS text.
- **Shadow / floor persistence** across save/reopen. Session-only, like the camera and the 6.5.3 light. **No** `.rpp`/SaveState wiring — leave [pcm_source_anim.cpp](../../src/pcm_source_anim.cpp) byte-for-byte unchanged.
- **A lit / textured / reflective floor.** The floor is a **flat unlit** plane + grid (its own minimal shader) that *receives* the shadow. It is not driven by the light-colour tool.
- **Multiple lights / coloured or area shadows / contact-hardening.** One directional shadow from the single key light, PCF-softened by quality level. No more.
- **Configurable grid/floor (spacing/colour/size) or shadow bias UI.** Auto-sized floor, in-code constants tuned at Antho's gate. No extra sliders.
- **Model self-shadowing is OPTIONAL** (see Dev Notes). The **required** receiver is the **floor**. If self-shadow on the mesh is straightforward once the infra exists, include it; if it introduces acne/artifacts that need tuning the dev can't do on Linux, leave it for the gate and ship floor-only shadows.

## Tasks / Subtasks

- [x] **Task 1 — Always-on floor geometry + flat-colour shader (AC1).**
  - [x] Add a dedicated **flat-colour floor shader** to [renderer.cpp](../../src/renderer.cpp) (separate program from the Blinn-Phong material shader). Vertex: outputs `gl_Position = u_mvp * vec4(a_pos,1.0)` **and** `v_worldpos = (u_model * vec4(a_pos,1.0)).xyz` (the world pos is needed to sample the shadow map in Task 3). Attributes: location 0 = `vec3 a_pos` only. Fragment: base colour `u_color`, multiplied by the shadow factor (Task 3); output **directly** (no `enc()`/sRGB, no lighting) — pick display-space greys tunable at the gate.
  - [x] Build the floor mesh **once** in `Init` (cold path, D2): a unit grid centred on origin in XZ at y=0 — a solid quad (`[-0.5,0.5]²`, 4 verts `GL_TRIANGLE_STRIP`) then `N×N` grid **lines** (e.g. 20 divisions, `GL_LINES`) packed into one `GpuBuffer`, drawn with two `glDrawArrays` (`0,4` strip; `4,gridVertCount_` lines). Dedicated `floor_vao_`. Store `gridVertCount_`.
  - [x] New `Renderer` members (RAII): `GpuProgram floor_program_;`, `GpuVertexArray floor_vao_;`, `GpuBuffer floor_vb_;`, `GLsizei gridVertCount_=0;`, floor uniform locations.
  - [x] **Floor placement (per frame, stack math):** `center=0.5f*(aabbMin+aabbMax)`; `extent=cam_.frameRadius*6.0f`; `M = translate(vec3(center.x, aabbMin.y, center.z)) * scale(vec3(extent,1,extent))`. Draw with **`GL_CULL_FACE` disabled** (visible from below) then re-enabled, and **`glPolygonOffset(1,1)` + `GL_POLYGON_OFFSET_FILL`** around the solid-plane draw so the grid lines don't z-fight (restore after). Depth test/write stay on; opaque.
  - [x] **Non-fatal (AR17):** floor-shader compile failure → `LogWarn` once, leave `floor_program_` empty, `Init` still returns `true`; gate the floor draw on `floor_program_.get()!=0`.

- [x] **Task 2 — Shadow-map depth pass (AC2, AC3, AC4).**
  - [x] Add `gl_loader.h` **FBO rows** (append-only to `RAV_GL_FUNCS`): `glGenFramebuffers`, `glDeleteFramebuffers`, `glBindFramebuffer`, `glFramebufferTexture2D`, `glCheckFramebufferStatus`, plus the matching `#define` lines; add the enums it needs (`GL_FRAMEBUFFER 0x8D40`, `GL_DEPTH_ATTACHMENT 0x8D00`, `GL_FRAMEBUFFER_COMPLETE 0x8CD5`, `GL_DEPTH_COMPONENT24 0x81A6`; `GL_DEPTH_COMPONENT`/`GL_NONE`/`GL_CLAMP_TO_EDGE`/`GL_FLOAT` come from `<gl/GL.h>` — `#ifndef`-guard any that don't). `glDrawBuffer`/`glReadBuffer` are GL 1.1 (direct, no row).
  - [x] New `Renderer` members (RAII): `GpuFramebuffer shadow_fbo_;` (add a `GpuFramebuffer = GpuHandle<FramebufferDeleter>` alias to [gpu_resources.h](../../src/gpu_resources.h) — **wait**, `gpu_resources.h` is *not* on the forbidden list, but to stay minimal you may instead hold the FBO as a raw `GLuint` deleted in `Shutdown`; prefer adding the RAII alias for symmetry with the other handles), `GpuImage shadow_depth_tex_;`, `ShadowQuality shadow_quality_ = ShadowQuality::Mid;`, `int shadow_map_size_ = 0;` (current allocation).
  - [x] **Allocate/realloc on quality change only** (cold path, in `SetShadowQuality`): pick size by level — **Off**=0 (no alloc/pass), **Low**=512, **Mid**=1024, **High**=2048. Create the depth texture (`glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT24,size,size,0,GL_DEPTH_COMPONENT,GL_FLOAT,nullptr)`, `GL_NEAREST`, `GL_CLAMP_TO_EDGE`), attach to `shadow_fbo_` (`GL_DEPTH_ATTACHMENT`), `glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);` (depth-only → no colour attachment), check `GL_FRAMEBUFFER_COMPLETE`. On any failure `LogWarn` once → fall back to Off (AR17/AC6).
  - [x] **Light-space matrix (per frame):** orthographic, fitted to the model. `lightPos = center + light_dir_ * (frameRadius*2)`; `lightView = lookAt(lightPos, center, up)` (guard `up` parallel to `light_dir_`); `lightProj = ortho(-r,r,-r,r, near, far)` with `r ≈ frameRadius*1.2` and near/far bracketing the model. `light_space_ = lightProj * lightView`.
  - [x] **Depth pass (skip entirely if quality==Off):** bind `shadow_fbo_`, `glViewport(0,0,size,size)`, `glClear(GL_DEPTH_BUFFER_BIT)`. **Reuse the main mesh program** with `u_mvp = light_space_ * model` and the already-computed pose/palette (compute the pose **once**, before this pass — see Dev Notes "RenderFrame re-order") so skinned models cast a posed shadow; colour output is discarded (no colour attachment). Optional: `glCullFace(GL_FRONT)` during the pass to cut acne, restored to `GL_BACK` after. **Restore** `glBindFramebuffer(GL_FRAMEBUFFER,0)` **and** `glViewport(0,0,width,height)` before the visible passes.

- [x] **Task 3 — Receive the shadow on the floor (and optionally the model) (AC2, AC3).**
  - [x] **Floor shader** samples the shadow map: add `uniform mat4 u_lightSpace; uniform sampler2D u_shadowMap; uniform float u_shadowOn; uniform int u_pcfRadius; uniform vec2 u_shadowTexel;`. Project `v_worldpos` into light space, perspective-divide, `*0.5+0.5`; if outside `[0,1]` → factor 1 (lit). Compare with bias (slope-scaled or a small constant ~0.0015) and **PCF**: loop `[-r,r]²` taps (r by quality: Low=0 → 1 tap; Mid=1 → 3×3; High=2 → 5×5), average. `shadow = mix(1.0, pcf, u_shadowOn)`; floor colour `*= mix(kShadowFloor, 1.0, shadow)` (e.g. `kShadowFloor≈0.45` so shadowed floor darkens, not to black). Bind `shadow_depth_tex_` to a free texture unit (e.g. unit 2 — units 0/1 are diffuse/normal on the mesh program, but the floor uses its own program so any unit is fine; pick 0 for the floor program) and set the uniforms; when quality==Off set `u_shadowOn=0`.
  - [x] **Optional model self-shadow:** the same sampling block in the main fragment shader, applied to the **diffuse** term only (keep ambient so shadowed faces aren't black). Gate behind a quick visual check — include if clean, else defer to the gate (see Out of scope).
  - [x] **`SetShadowQuality(ShadowQuality)` / `ShadowQuality()`** public on `Renderer` ([renderer.h:68-71](../../src/renderer.h#L68) area): the setter (re)allocates the map (cold path) and stores the level; `RenderFrame` reads it. `enum class ShadowQuality { Off, Low, Mid, High };` in `renderer.h`.

- [x] **Task 4 — Shadow tool in the Dear ImGui menu + new icon (AC2).**
  - [x] Add `'shadow'` to `names` in [tools/gen_icons.py:84](../../tools/gen_icons.py#L84) and **regenerate** `overlay_icons.h` (`python3 tools/gen_icons.py`) → adds `kIcon_shadow`. (Offline tooling — not a shipped file.) Commit the regenerated `overlay_icons.h` + `Icons/icon_shadow.svg`.
  - [x] Upload `kIcon_shadow` as a GL texture in `StartRendering` next to the other three ([viewer_window.cpp:549-551](../../src/viewer_window.cpp#L549)) → `g_icon_shadow`; free it in `StopRendering` ([viewer_window.cpp:583-585](../../src/viewer_window.cpp#L583)).
  - [x] In [DrawToolUi()](../../src/viewer_window.cpp#L385) `if (g_menu_open)` block, **remove any floor checkbox** (floor is always on) and add a **Shadows** section after Light: the shadow icon + label, then a **4-way selector** — recommended `ImGui::RadioButton("Off"/"Low"/"Mid"/"High", &g_shadow_quality, n)` in a row (or 4 `ImGui::Selectable`s) — and on change call `g_renderer.SetShadowQuality(static_cast<rav::ShadowQuality>(g_shadow_quality))`. Add `int g_shadow_quality = 2;` (Mid) to the UI-state globals ([viewer_window.cpp:130-134](../../src/viewer_window.cpp#L130)). Mirror the Light section's icon+label rhythm.

- [x] **Task 5 — Scope audit, gate §7, docs (AC4, AC5, AC6 + AR19/AR20).**
  - [x] `git diff --stat` shows only: `src/renderer.cpp`, `src/renderer.h`, `src/viewer_window.cpp`, `src/gl_loader.h` (FBO rows), `src/gpu_resources.h` (only if you add the `GpuFramebuffer` alias), `src/overlay_icons.h` (regenerated), `Icons/icon_shadow.svg`, `tools/gen_icons.py`. **No** `scene.h`/`CMakeLists.txt`/`plugin_main`/`reaper_api`/`pcm_source`/`asset_loader`; **no** `rec->Register`/`WANT_*`.
  - [x] **Update the [renderer.h](../../src/renderer.h#L1-L9) header comment**: it currently asserts the renderer "never binds an offscreen FBO". Shadow mapping introduces a transient offscreen **depth** FBO (always restored to FB0 for the visible output). Reword the comment and log this as an **AR20 Spec Change Log** deviation in [architecture.md](../planning-artifacts/architecture.md) (newest-on-top): Trigger (FR51 always-on floor + shadow-quality tool, Antho 2026-06-28) / Decision (flat floor shader + grid; shadow-map depth pass into an offscreen depth FBO — **deviates from the Spike-0 "FB0-only / no offscreen FBO" decision**, FBO bound only during the depth pass; 5 new gl_loader FBO rows [precedent 6.5.1/3.3]; reuse the main program for the depth pass; floor receives PCF shadow, model self-shadow optional; Off/Low/Mid/High = 0/512/1024/2048 + 1/1/9/25-tap PCF) / KEEP (AR15 register-symmetry, D2 zero-alloc on the per-frame path, AR17 non-fatal, session-only/no D9, Gate §7 PENDING).
  - [x] Append gate **§7** to [docs/PHASE4.5_VALIDATOR_GATE.md](../../docs/PHASE4.5_VALIDATOR_GATE.md), §6 shape: intro + `> Scope note (do NOT fail 6.5.4 for these)` (normal-map/MSAA toggles + FPS = 6.5.5; aliased grid lines / minor shadow acne at Low are OK; floor not persisted; model self-shadow may be absent) + `Suggested fixtures:` + a check table + `**Result:** … — PENDING`. **Never pre-mark PASS** (AR19).
  - [x] Update [deferred-work.md](deferred-work.md): mark the "Floor/grid (6.5.4)" line **DONE** (shipped, always-on); note the shadow lever realises part of FR52 (the rest — normal-map/MSAA toggles + FPS — stays 6.5.5); record "model self-shadow" if deferred.
  - [x] Build clean at `/W3 /permissive-` (NFR-R5). Linux can't build the `_WIN32` units / ImGui / open Reaper → self-review (GL FBO + PCF usage mirrored from standard shadow-mapping; ImGui radios mirror proven 6.5.3 widgets); judged at Antho's in-Reaper Windows gate (AR19).

## Dev Notes

### ⚠️ Two scope changes from the original spec (Antho, 2026-06-28)

1. **The floor is always on — drop the show/hide toggle.** No `g_floor_visible`, no `SetFloorVisible`. The renderer always draws the floor when an asset is loaded. This *simplifies* the floor half.
2. **The sidebar control is now a SHADOW tool, not a floor toggle** — Antho's new `Icons/icon_shadow.svg`, a 4-level **Off / Low / Mid / High** quality selector so a weak PC can dial shadows down/off. Off must cost nothing (skip the depth pass). This is the *substantial* half of the story.

### ⚠️ The UI is Dear ImGui (the epic's "Win32-child" language is stale)

Since 6.5.3 the tool menu is **vendored Dear ImGui** (frameless hamburger; Antho: "C'est parfait"). The shadow selector is **ImGui widgets** in [DrawToolUi](../../src/viewer_window.cpp#L385) (`RadioButton`/`Selectable`), not Win32 children, not ReaImGui.

### This is the heaviest rendering story of Epic 6.5 — be honest about the shape

Unlike 6.5.3 (UI-only, `renderer.cpp` net-zero) this story builds a **real-time shadow-mapping pipeline**: an offscreen depth pass from the light's POV + PCF sampling on the receiver. That is genuinely more than a floor toggle. It also **deviates from a binding architecture decision** — the renderer was specified to "never bind an offscreen FBO" ([renderer.h:1-9](../../src/renderer.h#L1)); shadow mapping requires one. That deviation is sanctioned here by Antho's direction and **must** be logged (AR20) and the header comment corrected. The FBO is bound only transiently for the depth pass; the visible output still goes to FB0.

### `RenderFrame` re-order (the one structural change to the hot path)

Today: clear FB0 → `if meshes.empty() return` → projection → view_proj → `glUseProgram(main)` + pose/palette + mesh loop. New order (all **after** the empty-return, so the idle panel is unchanged):
1. compute `view_proj_` (as today)
2. **compute the pose/palette once** (move the pose block [renderer.cpp:353-377](../../src/renderer.cpp#L353) up — both the depth pass and the visible pass need it)
3. **shadow depth pass** (skip if quality==Off): bind `shadow_fbo_`, set the small viewport, clear depth, draw the mesh loop with `u_mvp = light_space_*model`, restore FB0 + the window viewport
4. **floor pass** (always): bind `shadow_depth_tex_`, draw the floor with PCF shadow receive
5. **visible mesh pass**: as today (optionally with self-shadow sampling)
Keep the visible mesh loop's program/VAO/uniform setup intact — it re-binds and re-uploads after the floor pass switches programs, exactly as it must.

### Shadow-map specifics (Linux can't see it — get it right on paper)

- **Reuse the main program for the depth pass** (don't write a second skinning shader): set `u_mvp = light_space_*model`, the depth FBO has no colour attachment (`glDrawBuffer(GL_NONE)`), so the fragment colour is discarded and only depth is written. Skinned casters work because the palette is already uploaded.
- **Light-space ortho** fitted to `frameRadius`; rebuild every frame from the current `light_dir_` so the 6.5.3 light pad sweeps the shadow (AC3). Guard the `lookAt` up-vector when `light_dir_` is near-vertical.
- **Bias** to kill acne: a small constant (~0.0015) or slope-scaled (`max(b*(1-dot(N,L)), bmin)`); front-face culling in the depth pass also helps. Tunable at the gate — note the constant.
- **PCF by quality:** Off→no pass (factor 1); Low→1 tap (hard); Mid→3×3; High→5×5, stepping by `u_shadowTexel = 1/size`. `kShadowFloor≈0.45` so shadowed floor darkens but keeps tone.
- **Edge sampling:** `GL_CLAMP_TO_EDGE` + a shader bounds-check (outside `[0,1]` → lit) avoids needing border-colour enums.
- **State restore is mandatory:** the depth pass changes the bound FBO, the viewport, and (if used) the cull face — restore **all three** before the floor/mesh passes or the visible frame corrupts.

### GL boundary — `gl_loader.h` gets FBO rows (justified, precedented)

The floor needs only GL 1.1 + already-routed functions (`glDrawArrays`/`glLineWidth`/`glPolygonOffset`/cull toggle + the table's `glGenBuffers`/`glUseProgram`/`glUniform*`/…). **Shadows** additionally need GL 3.0 **FBO** entry points absent from the table — append them to the `RAV_GL_FUNCS` X-macro + the `#define` block ([gl_loader.h:61-130](../../src/gl_loader.h#L61)). Adding GL-loader rows is an established, append-only move (6.5.1 added a normal-map-era row; 3.3 added `glVertexAttribIPointer`). The AR15 *canonical* invariant is symmetric **Reaper** register/unregister — untouched. Log the rows in AR20.

### Non-fatal everything (AR17/AR18)

Floor- or shadow-shader compile failure, or an incomplete FBO → `LogWarn` once and fall back (no floor / no shadow), `Init` still returns true (only the **main** material shader's failure is fatal). Every floor/shadow draw is gated on its program/FBO being valid. The ImGui radios mutate state only; no synchronous render in a handler; no exception escapes `WindowProc` (AR18). Console is silent by default (6.5.2) — `LogWarn` is wired-but-silent; build `build_debuglog.bat` to see logs, `build.bat` to revert. No `printf`/`cout`/`OutputDebugString` (single-funnel `Emit()` invariant).

### Regression guardrails

- **Mesh/skinning/material visible path stays correct** — the depth pass reuses the program but always restores FB0/viewport/cull; the visible mesh loop re-binds and re-uploads as today. Epic 2/3/4 rendering is unchanged (modulo the optional self-shadow term).
- **Transport/playhead untouched** — shadows are pure render state.
- **Perf:** Off = zero added cost (no pass); Low must hold ≥60 fps at NFR-P1. Map/FBO allocated on quality change only (cold path) → D2 zero-alloc on the per-frame path preserved.
- **Hide/show + reopen:** quality (`g_shadow_quality` / `shadow_quality_`) is session-only; GL objects are RAII, recreated in `StartRendering`→`Init`/`SetShadowQuality` and freed in `Shutdown` (memory: docked close sends us nothing — don't destroy on hide; the timer parks while `!IsWindowVisible`).
- **Boundary (AR15):** only the files in Task 5's audit; no `rec->Register`/`WANT_*`/CMake/plugin_main/pcm_source/asset_loader/scene.h.

### Validation = Antho's in-Reaper Windows gate (AR19)

Linux compiles the non-`_WIN32` TUs but can't build the `_WIN32` renderer/viewer/ImGui units or open Reaper, so the floor+shadow pipeline is **self-reviewed** (standard shadow-mapping + PCF; GL usage mirrored from the proven mesh path; ImGui radios mirror 6.5.3 widgets) and judged **in-Reaper on Windows**. Author gate **§7** **PENDING** — never pre-mark PASS. Validator hooks are **click-based** (double-click `build.bat`, click the hamburger, click Off/Low/Mid/High, drag the light pad to watch the shadow move).

Suggested §7 checks (click-based): (1) a **floor + grid** is always under the model, at its feet, sized to it; (2) the menu shows a **Shadows** control with **Off/Low/Mid/High**; (3) at Mid the model casts a clean soft shadow on the floor — **no strobing/acne/peter-panning**; (4) dragging the **light pad** sweeps the shadow live; (5) **Low** looks harder, **High** softer; **Off** removes the shadow and the floor stays; (6) orbit/zoom/pan + Recenter + light tool still work; (7) **≥60 fps** holds at **Off and Low** during playback; close→reopen returns to the default level, no ghost/leak, console silent.

### Project Structure Notes

- Flat `src/`. This story: `src/renderer.cpp` (floor shader + grid build + shadow FBO/depth-texture + depth pass + PCF receive + `Shutdown` resets + `RenderFrame` re-order), `src/renderer.h` (`ShadowQuality` enum + members + `SetShadowQuality`/`ShadowQuality()` + corrected header comment), `src/viewer_window.cpp` (shadow icon upload/free + ImGui 4-way selector + `g_shadow_quality`; remove floor toggle), `src/gl_loader.h` (FBO rows + enums), `src/gpu_resources.h` (optional `GpuFramebuffer` alias), `src/overlay_icons.h` (regenerated), `Icons/icon_shadow.svg`, `tools/gen_icons.py` (+`'shadow'`). Docs: gate §7, AR20, deferred-work, sprint-status.
- Naming: members `snake_case_` (`floor_program_`, `shadow_fbo_`, `shadow_quality_`, `gridVertCount_`); globals `g_` (`g_icon_shadow`, `g_shadow_quality`); methods PascalCase (`SetShadowQuality`, `DrawFloor`); enum `ShadowQuality{Off,Low,Mid,High}`. Namespace `rav`; SPDX MIT header; WHY-only comments.

### References

- [Source: epics.md#Story-6.5.4 / FR51] — floor tool (revised to always-on by Antho 2026-06-28). [Source: epics.md#Story-6.5.5 / FR52] — graceful-degradation intent the shadow-quality lever partly realises (rest stays 6.5.5).
- [Source: epics.md:160-171] — AR15 (:163), AR17 (:165), AR18 (:166), AR19 (:170), AR20 (:171). [Source: architecture.md:93] — amended AR16 (menu is the sanctioned surface).
- [Source: 6-5-3-viewport-tool-sidebar-and-light-tool.md] — the Dear ImGui menu this extends (`DrawToolUi`, `ImGui::Image`/`Button`, icon upload at [viewer_window.cpp:549-551](../../src/viewer_window.cpp#L549)); the light pad drives `light_dir_` (which now also aims the shadow).
- [Source: src/renderer.h:1-9,68-71,113-125] — the "never binds an offscreen FBO" comment to revise, the inline-setter precedent, the light/uniform-location convention, `light_dir_`.
- [Source: src/renderer.cpp:164-179,183-251,291-442,444-453] — `CompileShader`, `Init` (fixed state + VAO), `RenderFrame` (early-return, view_proj, pose block to move up, mesh loop), `Shutdown`.
- [Source: src/gl_loader.h:32-56,61-130] — enum block + the `RAV_GL_FUNCS` X-macro to extend with FBO rows.
- [Source: src/gpu_resources.h:23-53] — `GpuHandle` template + deleters; add `FramebufferDeleter`/`GpuFramebuffer`.
- [Source: src/scene.h:90-91] / [src/camera.h:37,78-94] — AABB + `frameRadius` for floor placement, light-space fit, ortho sizing.
- [Source: tools/gen_icons.py:84,94] — `names=['menu','light','color']` → add `'shadow'` and regenerate. [Source: Icons/icon_shadow.svg] — Antho's new icon (untracked, commit it).

## Dev Agent Record

### Agent Model Used

claude-opus-4-8[1m] (Opus 4.8, 1M context) — dev-story execution 2026-06-28.

### Debug Log References

- Scope audit: `git diff --stat -- src/ tools/ Icons/` → only `gl_loader.h`, `gpu_resources.h`, `overlay_icons.h` (regenerated), `renderer.cpp`, `renderer.h`, `viewer_window.cpp`, `tools/gen_icons.py`, new `Icons/icon_shadow.svg`. `git diff -- src/ | grep -E 'rec->Register|REAPERAPI_WANT_'` → empty. No `scene.h`/`CMakeLists.txt`/`plugin_main`/`reaper_api`/`pcm_source`/`asset_loader` change.
- Isolated syntax check (the `_WIN32` renderer cannot build on Linux): `g++ -std=c++17 -fsyntax-only -D_WIN32 -Wall -Wextra` on `renderer.cpp` with stub `windows.h` + `gl/GL.h` (GL 1.1 surface) + vendored glm → **exit 0, no warnings**. This compiles the floor + shadow C++/type layer (uniform names, enum usage, RAII members, the RenderFrame re-order); the GLSL + GL-runtime behaviour is self-reviewed and judged at Antho's in-Reaper gate.
- Icon coverage check: `kIcon_shadow` rasterized to 58 % non-transparent pixels (non-blank; the gen_icons multi-subpath path renders Antho's overlapping-squares SVG correctly).

### Completion Notes List

- **Task 1 (floor):** new flat-colour floor program (separate from the Blinn-Phong shader) in `renderer.cpp` — solid quad (`GL_TRIANGLE_STRIP`, 4 verts) + a 20×20 grid (`GL_LINES`) packed into one `GpuBuffer`/`floor_vao_`, built once in `Init` (cold path). Placed per frame at `aabbMin.y`, centred, `extent = 6× frameRadius`, drawn cull-off + polygon-offset so the grid doesn't z-fight. Non-fatal build (LogWarn, skip the draw on failure; `Init` still true).
- **Task 2 (depth pass):** 5 append-only FBO rows in `gl_loader.h` (+ guarded enums) and a `GpuFramebuffer` RAII alias in `gpu_resources.h`. `GL_DEPTH_COMPONENT24` depth texture + FBO allocated **on a quality change only** (`SetShadowQuality` / `Init` default-Mid); the per-frame path is allocation-free (D2). The depth pass reuses the **main mesh program** (`u_mvp = lightSpace*model`, front-face cull, depth-only via `glDrawBuffer(GL_NONE)`); FB0 + viewport + cull are restored before the visible passes. Light-space ortho rebuilt each frame from `light_dir_` (AC3).
- **Task 3 (receive):** floor fragment samples the shadow map with **PCF** (Low 1 tap / Mid 3×3 / High 5×5), constant bias ≈0.0015, `CLAMP_TO_EDGE` + `[0,1]` bounds-check, `kShadowFloor ≈ 0.45`. New `Renderer::SetShadowQuality(ShadowQuality)` + `enum class ShadowQuality{Off,Low,Mid,High}` (default Mid). **Model self-shadow DEFERRED** (Out-of-scope-optional) to keep the gate-validated Epic-2/3/4 **mesh shaders byte-for-byte unchanged** — the required receiver (the floor) ships; recorded in `deferred-work.md`.
- **Task 4 (UI):** `'shadow'` added to `tools/gen_icons.py`, `overlay_icons.h` regenerated (`kIcon_shadow`); uploaded in `StartRendering`, freed in `StopRendering`. A 4-way `ImGui::RadioButton` Off/Low/Mid/High selector in `DrawToolUi` calls `SetShadowQuality`; `g_shadow_quality` defaults to 2 (Mid), matching the renderer default (no getter needed — a same-named `ShadowQuality()` getter would hide the enum type and break the out-of-line definition, so it was intentionally omitted).
- **Task 5 (audit/docs):** scope audit clean; `renderer.h` header comment reworded (visible output → FB0, transient depth FBO for the shadow pass); AR20 Spec Change Log entry (the offscreen-FBO deviation, Antho-directed); gate **§7** authored (Result **PENDING** — never pre-marked); `deferred-work.md` updated (floor DONE, self-shadow + remaining-FR52 deferred).
- **RenderFrame re-order:** pose/palette computed once → shadow depth pass (skip if Off) → floor pass → visible mesh pass (re-binds program/VAO, restores the visible `u_mvp`). Idle/empty panel unchanged (early-return before any of this). Boundary (AR15) intact; D2 zero-alloc per-frame preserved; AR17 non-fatal floor/shadow; session-only (no D9, `pcm_source_anim.cpp` untouched); NFR-R3 teardown (floor + shadow RAII freed in `Shutdown`).
- **Gate:** Antho's in-Reaper Windows visual + perf validation (`docs/PHASE4.5_VALIDATOR_GATE.md` §7) — the `_WIN32` renderer/ImGui units don't build on Linux and the render can't be seen, so the pipeline is self-reviewed (standard shadow-mapping + PCF; ImGui radios mirror 6.5.3).

### File List

- `src/renderer.cpp` — floor shader + grid build (`BuildFloor`), shadow FBO/depth texture (`AllocShadowMap`, `SetShadowQuality`), depth pass + PCF-receive floor draw (`DrawFloor`), `BindMeshAttribs` helper, RenderFrame re-order, Shutdown resets.
- `src/renderer.h` — `enum class ShadowQuality`, `SetShadowQuality`, private helpers + floor/shadow members + floor uniform locations, corrected header comment.
- `src/viewer_window.cpp` — `g_shadow_quality` + `g_icon_shadow`, icon upload/free, the 4-way Shadows selector in `DrawToolUi`.
- `src/gl_loader.h` — 5 append-only FBO function rows + `#define` routing + FBO/clamp enums.
- `src/gpu_resources.h` — `FramebufferDeleter` + `GpuFramebuffer` RAII alias.
- `src/overlay_icons.h` — regenerated (adds `kIcon_shadow`).
- `tools/gen_icons.py` — `'shadow'` added to the icon name list.
- `Icons/icon_shadow.svg` — Antho's new shadow-quality icon (committed).
- `docs/PHASE4.5_VALIDATOR_GATE.md` — gate §7 (PENDING).
- `_bmad-output/planning-artifacts/architecture.md` — AR20 Spec Change Log entry.
- `_bmad-output/implementation-artifacts/deferred-work.md` — 6.5.4 deferrals (floor DONE, self-shadow, remaining FR52).
- `_bmad-output/implementation-artifacts/sprint-status.yaml` — status → review.

### Change Log

- 2026-06-28 — Post-gate addition (Antho): **floor on/off toggle restored** (his new `Icons/icon_ground.svg`). A **Ground** checkbox in the tool menu drives a new `Renderer::SetFloorVisible(bool)` (default on); hiding the floor also **skips the shadow depth pass** (the floor is the only shadow receiver, so there is nothing to cast onto). This re-introduces the show/hide control the 2026-06-28 scope revision had removed (Antho changed his mind — he wants to be able to turn the ground off). `'ground'` added to `gen_icons.py`, `overlay_icons.h` regenerated (`kIcon_ground`); `renderer.{h,cpp}` + `viewer_window.cpp`; `-Wall -Wextra` syntax check clean. Session-only (no persistence). The sprint key's `…-show-hide` name is, fittingly, accurate again.
- 2026-06-28 — Post-gate polish (Antho feedback): the **floor now tints by the key-light colour** (`u_lightColor` passed to the floor shader, floor grey × light colour). Antho noticed the 6.5.3 light-colour tool appeared to "only recolour the model" — because the floor was a flat unlit grey that ignored the light (the original 6.5.4 scope: "floor not driven by the light-colour tool"). Tinting the floor makes coloured light read across the whole scene (a red light reddens the ground too). The floor stays otherwise flat/unlit (no spec/reflection); shadows unaffected. 4-line change (`renderer.cpp` floor fragment + `DrawFloor` uniform + `renderer.h` location); `-Wall -Wextra` syntax check clean. Supersedes the "not driven by the light-colour tool" out-of-scope line for the floor *tint* only.
- 2026-06-28 — Story 6.5.4 → **DONE**. Antho in-Reaper Windows gate **§7 PASSED** (AR19): the always-on floor + grid sits under the model, the model casts a real shadow onto it, the **Off/Low/Mid/High** selector takes effect live, the light pad sweeps the shadow, and **Off** removes the shadow while the floor stays. Gate §7 Result PENDING→PASS. Epic 6.5 stays in-progress (6.5.5 perf/FPS remains backlog).
- 2026-06-28 — Story 6.5.4 implemented → **review**. Always-on floor (flat-colour plane + 20×20 grid in its own program, scaled to the model AABB) + real-time **cast shadow** (offscreen depth-FBO shadow-mapping pass reusing the main program; floor receives PCF; **Off/Low/Mid/High** = skip/512/1024/2048 with 1/9/25-tap PCF) driven by a 4-way ImGui RadioButton in the 6.5.3 menu (Antho's new `icon_shadow`). **Deviates from the Spike-0 "no offscreen FBO" rule** (Antho-directed, AR20-logged; FBO bound only for the depth pass, FB0 restored for visible output) + 5 append-only `gl_loader.h` FBO rows (precedent 6.5.1/3.3). Model self-shadow deferred (floor-only) to keep the mesh shaders byte-for-byte unchanged. Session-only (no D9). First non-net-zero `renderer.cpp` change in Epic 6.5. AR15 register-symmetry intact (no Register/WANT_/CMake/scene.h/plugin_main/pcm_source/asset_loader); D2 zero-alloc per-frame (maps alloc on quality change only); AR17 non-fatal floor/shadow; NFR-R3 RAII teardown. Isolated `-Wall -Wextra` syntax check of `renderer.cpp` clean. Gate §7 PENDING (Antho in-Reaper Windows, AR19).
- 2026-06-28 — Story 6.5.4 created (ready-for-dev). Original floor show/hide replaced (Antho) by: **always-on floor** + a **shadow-quality tool (Off/Low/Mid/High)** — the model casts a PCF shadow onto the floor, dialable down/off for weak PCs. Floor = flat-colour shader + grid in `renderer.{h,cpp}`. Shadows = a depth-pass shadow-mapping pipeline (offscreen depth FBO → **deviates from the Spike-0 FB0-only decision**, AR20-logged; +5 FBO rows in `gl_loader.h`, precedent 6.5.1/3.3; reuses the main program for the depth pass; floor receives, model self-shadow optional). New `Icons/icon_shadow.svg` (regen `overlay_icons.h`). ImGui 4-way selector in the 6.5.3 menu. Session-only (no D9). First non-net-zero `renderer.cpp` change in Epic 6.5. AR15 register-symmetry intact (no Register/WANT_/CMake/scene.h/plugin_main/pcm_source/asset_loader). D2 zero-alloc per-frame (maps alloc on quality change). Gate §7 PENDING. Depends 6.5.3.
