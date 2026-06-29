# Deferred Work

Review findings and follow-up items surfaced during quick-dev iterations
that are intentionally deferred to a later phase. Each item has a clear
trigger condition for when it should be picked up.

---

## Deferred from: story 6.5.4 (always-on floor + shadow-quality tool) — 2026-06-28

Story 6.5.4 shipped the **always-on floor + grid** (no toggle — it is part of the scene) and a real
**cast shadow** from the model onto the floor with a 4-level **Off / Low / Mid / High** quality selector
(realising FR51 + the **graceful-degradation** slice of FR52). A few adjacent items were deliberately
left out — recorded here so they are not silently dropped.

- **✅ Floor / grid (FR51) — DONE (shipped, always-on).** The prior 6.5.1 deferral "Floor/grid (6.5.4) … is not here" is now **realised**: a flat-colour ground plane + grid in its own minimal program (`renderer.cpp`), built once in `Init`, scaled to the model AABB per frame, drawn under the model. Kept here struck-through-equivalent for history; no longer deferred.
- **Model self-shadowing is DEFERRED (floor-only cast shadow shipped)** [`src/renderer.cpp` main fragment shader]. The story made model self-shadow **optional** ("include if clean, else defer to the gate"). It was deferred so the **gate-validated Epic-2/3/4 mesh shaders stay byte-for-byte unchanged** (self-shadow would add a sampling block + bias-tuning risk to the main shader that cannot be judged on the Linux box). The **required receiver — the floor — works**; the model's own surface does not yet self-shadow. **Trigger:** if Antho wants the model to shadow itself, add the same PCF sampling block to the main fragment shader applied to the **diffuse term only** (keep ambient so shadowed faces aren't black), gated behind a quick visual check for acne; the depth pass + light-space matrix + shadow texture it needs **already exist** (no new infra).
- **✅ DONE (Story 6.5.5, 2026-06-28) — Remaining FR52 levers (normal-map / MSAA quality toggles) + FR53 on-canvas FPS** [`src/viewer_window.cpp` `DrawToolUi`; `src/renderer.*`]. 6.5.4 pulled only the **shadow** quality lever forward; it added **no** normal-map switch, **no** MSAA, **no** FPS text. **Delivered by 6.5.5:** a **Performance** section in the 6.5.3 menu with **Normal maps** (one-uniform AND-gate of the per-material `u_hasNormalMap` flag — no GLSL change), **MSAA** (one-time `wglChoosePixelFormatARB` 4×-multisample format + verbatim legacy `ChoosePixelFormat` fallback + live `glEnable/glDisable(GL_MULTISAMPLE)`; checkbox disabled if the driver offers no multisample format), and an **FPS** readout (top-right overlay reading the still-running 6.5.2 measurement). Gate §8 PENDING. **⚠️ MSAA portion SUPERSEDED by Story 6.5.6 (2026-06-29):** the 6.5.5 on/off MSAA (baked window multisample format + `glEnable/glDisable(GL_MULTISAMPLE)`) couldn't change level live and was driver-ignored ("no visible difference" at Antho's test) → replaced by a **selectable Off/2×/4×/8× level** via an **offscreen multisample colour FBO + blit-resolve** (window reverts to single-sample). The **Normal-maps toggle + FPS readout are unchanged** by 6.5.6. See §6.5.6 below.

## Deferred from: story 6.5.6 (selectable MSAA quality levels) — 2026-06-29

Story 6.5.6 shipped a **games-style MSAA level selector** (Off / 2× / 4× / 8×, clamped to `GL_MAX_SAMPLES`)
via an **offscreen multisample colour FBO + blit-resolve to the window**, replacing 6.5.5's on/off MSAA.
The deferrals it records:

- **MSAA level persistence across save/reopen** — the level is **session-only** like the camera / light /
  shadow / floor; a fresh viewer opens at the default (clamped 4×). **Trigger:** post-MVP, if a per-project
  "remembered render settings" feature is wanted — it would wire the level (and the other session-only
  levers) through `.rpp`/SaveState/D9. Out of scope here to keep `pcm_source_anim.cpp` byte-for-byte unchanged.
- **SSAA / FXAA / post-process AA, per-pass AA, and an sRGB-aware resolve (`GL_FRAMEBUFFER_SRGB`)** — 6.5.6
  is **MSAA via offscreen multisample resolve only**; the offscreen colour buffer is plain `GL_RGBA8` (the
  mesh shader already encodes sRGB on its final write — an sRGB resolve would double-encode). **Trigger:**
  post-MVP AA/colour-pipeline polish (the same entry as the full-PBR/tone-mapping deferral below).
- **A custom MSAA icon** — the selector lives inside the existing **Performance** section (Antho's
  `icon_performance`, 6.5.5); no new icon was added.
- **Floor / shadow look not configurable + not persisted** [`src/renderer.cpp` in-code constants; D9 untouched]. The floor greys, size (`6× frameRadius`), grid divisions (20), shadow bias (`≈0.0015`), `kShadowFloor` (`≈0.45`), and shadow-map sizes (512/1024/2048) are **in-code constants tuned at the gate** — no UI sliders. Shadow quality is **session-only** (resets to Mid on reopen; `pcm_source_anim.cpp` untouched). A lit/textured/reflective floor, multiple/coloured/area shadows, and contact-hardening are out of scope. **Trigger:** only if Antho asks for a configurable/persisted floor or richer shadows at a gate — each is a localized constant→uniform/SaveState change, deliberately scoped out of the MVP viewport.

---

## Deferred from: story 6.5.3 (viewport tool sidebar + light tool) — 2026-06-28

Story 6.5.3 built the top-right tool strip + the light tool (colour + azimuth/elevation). A few
adjacent ideas were deliberately left out — recorded here so they are not silently dropped.

- **Light `u_ambient` slider in the UI** [`src/renderer.h:112` `ambient_`; future flyout control]. The light tool's AC (FR50) names only `u_lightColor` and `u_lightDir`; `ambient_` keeps its 6.5.1 default `0.35`. **Trigger:** only if Antho asks for an ambient/fill control at a gate — it is a one-line setter + one flyout button mirroring the colour/position controls (a deliberate scope line, not an oversight).
- **Light-state persistence across save/reopen** [`src/viewer_window.cpp` `g_light_azimuth`/`g_light_elevation` + the renderer light members; D9 `SaveState`/`LoadState`]. The light is **session-only**, exactly like the camera — it resets to the 6.5.1 default on reload, with no `.rpp`/SaveState wiring (D9 is the per-item surface, untouched here). **Trigger:** if light look needs to survive a project round-trip, fold it into the D9 SaveState payload (decide per-item vs global first — the camera is global today).
- **~~Icon glyphs on the ImGui tool buttons~~** — **DONE in 6.5.3 rev #3 (2026-06-28).** Antho's `Icons/icon_menu.svg` / `icon_light.svg` / `icon_color.svg` are rasterized offline by `tools/gen_icons.py` → `src/overlay_icons.h` (white+alpha RGBA), uploaded as GL textures at ImGui init and drawn via `ImGui::ImageButton`/`ImGui::Image`. No longer deferred — kept here struck-through for history.
- **Dock the ImGui panel into Reaper's docker** [`src/viewer_window.cpp`]. The "Tools" window currently floats inside the viewport (draggable). **Trigger:** if Antho wants it as a separate dockable controls panel, enable ImGui docking (docking branch) or host it differently.

> _Note:_ all earlier 6.5.3 UI deferrals (native +/- buttons vs trackbars, owner-draw strip icons, a hand-rolled GL text renderer) are **obsolete** — the native-control and GL-overlay UIs were both replaced wholesale by vendored Dear ImGui (see architecture AR20 entry "Tool UI built on vendored Dear ImGui", 2026-06-28). The `u_ambient` slider and light-state persistence deferrals above still stand.

---

## Deferred from: code review of story 6.5.2 (2026-06-28)

- **`build_forcefail.bat:21` carries the same `>/dev/null` Unix-ism as the new `build_debuglog.bat`** [`build_forcefail.bat:21` — `where cmake >/dev/null 2>nul`]. In cmd.exe this redirects stdout to a non-existent `\dev\null` path; the canonical `build.bat` uses `>nul 2>nul`. The 6.5.2 review patches `build_debuglog.bat`; `build_forcefail.bat` is a pre-existing untracked dev-only build script (force-fail story) so it is left untouched here. **Trigger:** next time `build_forcefail.bat` is edited (or the force-fail validator path is exercised), align its `where cmake` redirect to `>nul 2>nul`.

---

## Deferred from: story 6.5.2 (silence all console logging by default) — 2026-06-28

Story 6.5.2 silenced the `[RAV]` console channel (the `Emit()` funnel body compiled out by
default). Two genuinely user-facing signals lost their console output and are **rehomed
on-canvas in Story 6.5.5** — recorded here so neither is silently dropped. Both depend on
the on-canvas **text-overlay mechanism** that does not exist until the sidebar work in
6.5.3/6.5.5; building a one-off overlay now (before that mechanism exists) would be throwaway,
so they are co-located in 6.5.5 sharing one overlay. **Ship-safe:** Epic 6.5 is pre-ship and
6.5.5 lands before Epic 7, so the on-canvas signals exist by ship; the only signal-less window
is **dev-time between 6.5.2 and 6.5.5**.

- **✅ DONE (Story 6.5.5, 2026-06-28) — On-canvas FPS readout (FR53), rehome of the silenced FPS log** [`src/viewer_window.cpp`]. 6.5.2 silenced the FPS `LogInfo` at the funnel but **left the measurement intact** (`QueryPerformanceCounter` + `g_frame_count` + the `elapsed >= 1.0` cadence) precisely because 6.5.5 reuses it. **Delivered:** the roll-up now stores `g_fps`; a second frameless `NoInputs` ImGui overlay shows it **top-right** when the **FPS** checkbox is on (default off). The silent `LogInfo` is kept (debug-build only); **no** console FPS was re-added.
- **✅ DONE (Story 6.5.5, 2026-06-28) — Minimal on-canvas load-failure indication, rehome of the silenced load-failure log** [`src/viewer_window.cpp`]. 6.5.2 silenced the load-failure `LogError`; its load-bearing **gate-advance-on-failure** logic (AR17 — stops a per-frame retry storm) is **unchanged**. **Delivered:** the `LogError` site also stores `g_load_error` + a ~4 s expiry; a third frameless `NoInputs` overlay shows "Failed to load: <category>" near the top while fresh, then disappears. No popup (AR16); honours 6.5.2 AC3's "not orphaned".

---

## Deferred from: story 6.5.1 (source-fidelity rendering — sRGB + lighting + specular + normal maps) — 2026-06-27/28

- **Alpha / transparency blending is NOT implemented (its own feature/story)** [`src/renderer.cpp` draw loop; `src/scene.h` `SceneMaterial`]. Surfaced at the 6.5.1 GLB gate: the steampunk-explorer GLB has two glTF `alphaMode: "BLEND"` materials (a `Window` at opacity 0.11, a `LightBulb_a` at 0.5) which we render **fully opaque** (no `GL_BLEND`, no alpha read, depth-write on), so glass/windows show as solid panels. 6.5.1 is sRGB/lighting/specular/normal — opacity is out of scope. **Trigger:** when transparent assets matter, add a transparency pass — read material opacity (`AI_MATKEY_OPACITY`) + base-colour alpha into `SceneMaterial`, sort/draw `BLEND` materials after opaque with `glEnable(GL_BLEND)` + `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` and depth-write off (and decide `MASK`/cutout handling). Its own story (interacts with draw order / the §C un-baked skinned path).
- **Energy-normalized Blinn-Phong is not a real microfacet BRDF** [`src/renderer.cpp` fragment shader]. The `(n+8)/(8π)` normalization stops rough metals washing out, but it is still Blinn-Phong, and a glTF **metal still shows its base-colour TEXTURE as diffuse** (a true metal has no diffuse; its colour belongs in the specular/reflection). Acceptable for a viewer; not physically correct. **Trigger:** folded into the post-MVP metallic-roughness Cook-Torrance/GGX + IBL upgrade below.
- **Normal mapping uses a per-fragment derivative tangent frame, not glTF-provided tangents** [`src/renderer.cpp` fragment shader]. glTF can ship a `TANGENT` attribute; we ignore it and rebuild the frame from screen-space derivatives (robust to mirrored UVs, format-uniform). Visually equivalent for normal mapping; slightly more ALU per fragment. **Trigger:** none for the MVP — only revisit if a future effect needs an exact authored tangent basis (e.g. anisotropic specular), in which case read `TANGENT` when present and fall back to derivatives.

- **The tool sidebar + light tool UI is Story 6.5.3, NOT built here** [`src/viewer_window.cpp` (future); `src/renderer.*`]. 6.5.1 only **introduces the light uniforms** `u_lightColor` / `u_lightDir` / `u_ambient` with hardcoded defaults stored as `Renderer` members; it builds **no** Win32-child icon strip, colour picker, or flyout. **Trigger:** Story 6.5.3 (depends on this story's uniforms) — add the top-right vertical icon strip (extend the Win32-child "Reset View" button pattern, **no** ReaImGui) + a light-tool flyout that drives `light_color_` / `light_dir_` (and the ambient) live; the uniforms + per-frame `glUniform*` plumbing are already in place to receive them.
- **Console logs are still present — removal is Story 6.5.2** [`src/console_log.*`; all `Log*` call sites incl. the FPS log at `src/viewer_window.cpp:237`]. 6.5.1 left `console_log.*` and every `Log*` call exactly as-is (incl. the per-texture unresolved warnings it adds via the existing `LogWarn` funnel). **Trigger:** Story 6.5.2 — silence all `[RAV]` logging by default via a single `Emit()` no-op funnel (re-enablable in a debug build), amending AR16; rehome the FPS + load-failure signals on-canvas (6.5.5 / a minimal indicator) so they are not orphaned.
- **Floor/grid (6.5.4) and render-quality toggles + on-canvas FPS (6.5.5) are not here** [`src/renderer.*`; `src/viewer_window.cpp` (future)]. 6.5.1 added **no** runtime "disable normal maps / MSAA" switch — normal mapping is gated purely on the **presence** of a normal map; there is no floor plane, no grid, no FPS readout, no MSAA. **Trigger:** Stories 6.5.4 (sidebar floor icon → solid ground plane + grid, session-only) and 6.5.5 (sidebar perf tool → toggle costly elements incl. normal maps/MSAA/floor + an on-canvas FPS readout replacing the removed console FPS log); both depend on the 6.5.3 sidebar infra.
- **Full PBR / metallic-roughness BRDF, IBL, shadows, tone-mapping, MSAA are post-MVP** [`src/renderer.cpp` shaders]. 6.5.1 keeps **Blinn-Phong** (architecture: "richer-than-Blinn PBR remains post-MVP") and a gamma-2.2 approximation of the sRGB transfer; it is sRGB + balanced lighting + dielectric specular + normal-map **sampling**, nothing more. The specular is a tuned Blinn-Phong lobe (`kSpecStrength`), not an energy-conserving microfacet BRDF; there is no image-based lighting, no shadow pass, no HDR tone-map, no multisample anti-aliasing. **Trigger:** if post-MVP polish wants photoreal parity, replace the Blinn-Phong block with a metallic-roughness microfacet BRDF (Cook-Torrance GGX) + IBL, add an MSAA-capable framebuffer (which would also revisit the `GL_FRAMEBUFFER_SRGB` decision — an FBO with an sRGB-capable attachment could move the encode off the shader), and a tone-mapping operator; MSAA specifically is a **6.5.5** quality toggle for the near term. **⏩ PULLED FORWARD (2026-06-29, Story 6.5.7):** the **per-pixel specular + glossiness map** slice of this is being implemented before ship — the loader will consume `aiTextureType_SPECULAR` + `aiTextureType_SHININESS` (the artist's maps, e.g. Mixamo `*_Specular.png` / `*_Glossiness.png`) to modulate the specular lobe per-pixel (intensity + exponent), keeping the dielectric base and **still ignoring the flat `COLOR_SPECULAR`**. It fixes the pronounced head/body seam on multi-material Mixamo FBX. The rest (Cook-Torrance/GGX microfacet BRDF, IBL, tone-mapping, true metal-diffuse suppression) stays post-MVP. See `sprint-change-proposal-2026-06-29-spec-gloss-maps.md`.
- **Height/bump maps under `aiTextureType_HEIGHT` are NOT used as normal maps (height→normal / slot-disambiguation is post-MVP)** [`src/asset_loader.cpp` `ResolveAndUploadNormalMap`]. Gate iteration 2026-06-27: the first build tried `aiTextureType_NORMALS` then fell back to `aiTextureType_HEIGHT`; on a downloaded FBX character whose relief map sits in the legacy FBX "bump" slot (a **grayscale height map**), sampling it as a tangent-space normal (`rgb*2-1` → ~`(2g-1,2g-1,2g-1)`) made the shaded normal track the height iso-lines → broad symmetric "melted-wax / contour-banding" smears. The HEIGHT fallback was **removed**; only true tangent-space normal maps (`aiTextureType_NORMALS`) are sampled. **Concrete evidence (2026-06-28, Mixamo "Vampire" `Hip Hop Dancing.fbx`, offline assimp probe):** `Vampire_MAT1` (the **body** — dominant surface) carries a proper `NORMALS` map → it **is** applied (so the body's flatness was lighting, fixed by the live light knobs — see below). But the secondary `Vampire_MAT_Transparent` material puts the **same `Vampire_normal.png` in the `HEIGHT` slot** — a *real normal map mis-slotted by the FBX exporter*, which we currently skip → that material gets no relief. So HEIGHT is **ambiguous across models** (grayscale bump on one, mis-slotted normal map on another); a blind fallback can't be right for both. **Trigger:** when micro-relief from HEIGHT-slot maps matters, add a load-time **disambiguation** — sample the HEIGHT texture and treat it as a tangent-space normal map only if it reads like one (predominantly blue, `mean(B) ≫ mean(R),mean(G)`), else run a **height→normal (Sobel)** conversion; never feed raw height bytes as a normal.

- **Live lighting-quality knobs added (ambient / specular / normal-relief), 2026-06-28** [`src/renderer.{h,cpp}` shader + `src/viewer_window.cpp` Light submenu]. Antho's "render looks flat/cheap vs Mixamo" feedback: the offline probe confirmed the body's normal map IS applied, so the cause was **flat lighting** (ambient 0.35 washing out form, near-zero dielectric specular). Fix: `u_specStrength` + `u_normalStrength` became uniforms (`kSpecStrength` was a shader const), defaults nudged to more contrast (ambient 0.35→0.20, spec 0.35→0.55, relief 1.0→1.4), and three live `SliderFloat`s (Ambient / Specular / Relief) added to the Light submenu so Antho tunes the final look in-Reaper. **Trigger / open:** the *defaults* are an educated guess (Linux can't see the render) — Antho dials them at the next build; if a specific value set wins, bake it as the default. A future per-light/per-material lighting model (proper PBR) is still the post-MVP path above.
- **The exact sRGB transfer is approximated by gamma 2.2** [`src/renderer.cpp` fragment shader `pow(c, 1/2.2)`]. The shader uses the accepted gamma-2.2 approximation rather than the piecewise sRGB transfer (linear segment near black + 2.4 exponent). Visually indistinguishable for this gate. **Trigger:** if a validator finds shadow-region banding/contrast off versus the source, swap in the exact piecewise sRGB encode (and matching decode if ever moving off `GL_SRGB8_ALPHA8`).

---

## Deferred from: story 6.3 (cross-machine portability via native Reaper media) — 2026-06-27

- **A custom relative-path / missing-media remap engine is explicitly NOT built (the defining boundary of 6.3)** [`src/pcm_source_anim.cpp`; would-have-been `project_state.{h,cpp}`]. FR46 was reformulated 2026-06-27 **off** a home-grown "relative path + missing-media remap" plan **onto** Reaper's **native** copy-into-project + relink (the path rides our file-backed `PCM_source` via `GetFileName`/`SetFileName` — already in place). So 6.3 deliberately builds **no** path-rewriting, search-path resolver, or "locate missing media" dialog of our own — cross-machine portability is native. **Trigger:** there is **no trigger to build a remap engine** — native copy/relink is the sanctioned mechanism. The only sanctioned code is the **only-if-gap** native-participation fix below; anything beyond "make the source participate in the *native* mechanism" is out of scope for the MVP.
- **A "locate missing media" UX is post-release** [`src/viewer_window.cpp`; `src/asset_loader.cpp`]. An unresolved path today surfaces a **console diagnostic** (`LoadAsset`→`LogError`, AR16) and isolates the failure (the poll advances the gate on failure so the rest of the session keeps playing — AR17 / FR37); there is **no** interactive relink/browse dialog. **Trigger:** if validators want a user-facing way to relocate a missing file from inside the viewer, design it post-release (Epic 8 robustness) on top of the existing `LoadResult{category,message}` path — still **not** a remap engine, and Reaper's own "locate media" already covers native relink.
- **Native-participation fix in `pcm_source_anim.cpp` fires only if the in-Reaper gate finds a gap** [`src/pcm_source_anim.cpp` `GetFileName`/`IsAvailable`]. The verify-first hypothesis (AR20) is that "copy media into project directory" already embeds our file-backed items and native relink already finds a moved file → **expected zero-code**. **Trigger:** if `docs/PHASE4_VALIDATOR_GATE.md` §6 row 2 shows a moved-folder reopen does **not** relink + play our items, diagnose *which* native step failed — (a) "copy media" did not copy our files, or (b) relink did not call `SetFileName` on the moved path — and apply the **minimal** fix so Reaper treats the source as copyable/relinkable (`GetFileName`/`IsAvailable` reporting), behind the AR18 no-throw boundary, **without** weakening the `file=` `only-if-empty` guard (that guard keeps the native/relinked path authoritative — the load-bearing 6.1↔6.3 interaction). Re-run §6 after any fix.
- **Per-item camera framing + `time_offset`/`time_scale` reaffirmed-deferred (unchanged from 6.1/6.2)** [`src/pcm_source_anim.cpp`; `src/renderer.*`]. 6.3 persists nothing new: camera is **global** today (no per-item source of truth), and `time_offset`/`time_scale` are **native** take state (`D_STARTOFFS`/`D_PLAYRATE`) Reaper persists and relocates itself. **Trigger:** unchanged from the 6.1 entries below (`D_PLAYRATE` per its 4.4 trigger).

---

## Deferred from: story 6.2 (panel/viewport dock-state persistence) — 2026-06-27

- **Per-item camera framing is still NOT persisted (reaffirmed from 6.1)** [`src/renderer.*` `OrbitCamera`; `src/pcm_source_anim.cpp` `SaveState`/`LoadState`]. The epic AC for 6.2 names "an adjusted camera" in its *Given*, but the camera is **global** today (the renderer owns one `OrbitCamera`, reset on every `SetAsset` — Story 2.4), so there is **no per-item camera source of truth** to persist; the testable *Then* for 6.2 is **dock position only**. `cam_*` keys are **not** added to `SaveState`/`LoadState`. **Trigger:** unchanged from the 6.1 entry below — make the camera per-item (per-item `AnimSource` fields, viewer writes on change / reads+applies on item switch), then add the `cam_*` keys; the versioned `rav_ver`/`key=value` framework already carries them.
- **Auto-reopen the viewer panel on project load is NOT implemented** [`src/viewer_window.cpp`; `src/plugin_main.cpp`]. 6.2 relies on the **dock position** (native docker / `kDockIdent` in `reaper.ini`), but it does **not** make the viewer panel **auto-open** when a project that had it open is reopened — the user re-runs the `RAV: Open Viewer` toggle action, and the panel returns to its remembered dock. Dock *position* is global (`reaper.ini`); "was the panel open in *this* project" is per-project state we do not currently record. **Trigger:** if validators want the panel to reappear automatically on project load, persist a per-project "viewer was open" flag (project ext-state or a `ProjectStateContext` hook) and, on a project-load notification, call `OpenViewerWindow` while keeping the toggle/`RefreshToolbar` state coherent (AR15/AR18) — **never** ReaImGui, **never** a custom screenset engine.
- **Global ext-state dock fallback fires only if the in-Reaper gate finds a gap** [`src/viewer_window.cpp`; `src/reaper_api.h`]. The verify-first hypothesis (AR20) is that the native docker (`DockWindowAddEx` + stable `kDockIdent`) already round-trips the dock position (dock state is Reaper-global / `reaper.ini`, not per-project, per D9) → **expected zero-code**. **Trigger:** if `docs/PHASE4_VALIDATOR_GATE.md` §5 row 1/2 shows the panel does **not** return to its remembered dock, add a **minimal** global `SetExtState`/`GetExtState` save/restore in `viewer_window.cpp` (+ `REAPERAPI_WANT_SetExtState`/`WANT_GetExtState` in `reaper_api.h`, resolved via the existing `REAPERAPI_LoadAPI` — no new `rec->Register`), behind the AR18 no-throw boundary, and re-run §5.

---

## Deferred from: story 6.1 (per-item state persistence) — 2026-06-27

- **Per-item camera framing is NOT persisted (camera is global today)** [`src/pcm_source_anim.cpp` `SaveState`/`LoadState`; `src/renderer.*` `OrbitCamera`]. The D9 `key=value` format names optional `cam_target` / `cam_distance` / `cam_yaw` / `cam_pitch` keys ([architecture.md:589-596](../planning-artifacts/architecture.md#L589)), but Story 6.1 persists **none** of them, because there is **no per-item camera state to persist**: the renderer owns **one** `OrbitCamera` (`cam_`), reset on every `SetAsset` (Story 2.4), shared across all items. Writing `cam_*` now would have no source of truth. **Trigger:** if per-item camera framing is wanted, (1) add per-item camera fields to `AnimSource`, (2) have the viewer **write** the current camera into the current item's source on change and **read+apply** it on item switch (the same poll/load path 4.3–4.5 uses), then (3) add the `cam_*` keys to `SaveState`/`LoadState` — the versioned `rav_ver`/`key=value` framework is already in place to carry them (forward-compat: an older extension ignores the new keys).
- **`time_offset` / `time_scale` are native take state, not ours to write** [`src/pcm_source_anim.cpp` `GetCurrentAnimItem`]. The D9 format also lists optional `time_offset` / `time_scale` keys, deliberately **not** persisted by 6.1: left-trim offset is take **`D_STARTOFFS`** — Reaper persists it **natively** and 4.4's `GetCurrentAnimItem` already consumes it; writing our own `time_offset` would duplicate native state and risk the same override hazard the `file=` `only-if-empty` guard exists to prevent. Playrate is take **`D_PLAYRATE`** — **already deferred from Story 4.4** (cross-reference the 2026-06-27 story-4.4 entry below; not duplicated here). **Trigger:** none from 6.1 — these stay native; only revisit `D_PLAYRATE` per its existing 4.4 trigger (retime the animation by multiplying the item-relative term by `D_PLAYRATE`, reconciled with 4.2 item-sizing).

---

## Deferred from: code review of story-4.5 (2026-06-27)

- **Half-open span `[ip, ip+il)` — playhead parked exactly on a lone item's end shows nothing** [`src/pcm_source_anim.cpp` `GetCurrentAnimItem`, the `pos >= ip + il` continue]. Pre-existing 4.3/4.4 boundary, unchanged by 4.5's selection rework. For two abutting items the half-open span is correct (the next item owns that sample); but for a **single** item, when the cursor lands exactly on `ip+il` the function returns false. With the startup fixture gone (if the `viewer_window.cpp` removal is kept), the first-ever interaction parking precisely on an end edge shows a blank panel instead of the last frame. **Trigger:** if validators report a one-sample blank at item ends, decide whether the end sample should hold the item (close the high end with `pos <= ip + il` for the lone-item case, or hold-last-frame in RenderTick).
- **Current item deleted/moved → viewer holds a stale frame of a no-longer-present asset** [`src/viewer_window.cpp` `RenderTick` hold branch + the `g_transport_driven` latch]. Once any RAV item drives the view, `g_transport_driven` latches true forever; deleting the current item (or removing the topmost overlapping item with nothing else spanning) makes `GetCurrentAnimItem` return false → RenderTick HOLDs the deleted item's last frame, never reverting to the idle/blank background. Pre-existing (AC3 "freeze on leave"), but it conflicts with the "viewport idle until an item is current" narrative introduced by the picker/fixture removal. **Trigger:** if a validator finds a deleted animation still frozen on screen and wants it cleared, reset `g_transport_driven`/clear the displayed asset when no item has been current for N ticks (or on a project-change notification).
- **"Ours"-typed topmost item with an empty source path masks a valid lower-track item** [`src/pcm_source_anim.cpp` winner-emit, `out_path = fn ? fn : ""`]. 4.5 selects the winner by track number only; a topmost RAV item whose `GetFileName()` is null/empty would set `out_path=""` and suppress an otherwise-renderable lower-track item (the old first-match code had the same exposure only for the first item). Realistically unreachable — our `PCM_source` always carries `m_path` — and `LoadAsset("")` fails+logs once (path-change gate, no per-frame storm). **Trigger:** if an empty-path "ours" source ever becomes possible, skip empty-path candidates in the walk so a valid lower-track item can win.

*(The same-track Z-order tie-break and the duplicate-file asset cache are recorded under the story-4.5 implementation deferrals below, not duplicated here.)*

---

## Deferred from: story 4.5 (multi-item / current-item selection) — 2026-06-27

- **Same-track overlap front-most / Z-order precedence is not honored** [`src/pcm_source_anim.cpp` `GetCurrentAnimItem`]. Story 4.5 resolves overlap by **track** priority (the topmost track's spanning RAV item wins — lowest `IP_TRACKNUMBER`), satisfying FR14's "highest-priority **track**". But when **two** RAV items overlap on the **same** track (same `IP_TRACKNUMBER`), the selection ties and the walk keeps the **first walk-order** item (strict `<`), which is deterministic but is **not** necessarily Reaper's visual **front-most / Z-order** item. **Trigger:** if a sound designer stacks RAV items on a single track (e.g. in lanes / fixed-item-lanes) and expects the visually front-most to display, read the item Z-order / lane info and add it as a secondary tie-break in `GetCurrentAnimItem` (after the `IP_TRACKNUMBER` compare).
- **No asset cache to share VRAM across duplicate files** [`src/pcm_source_anim.cpp` / `src/viewer_window.cpp` reload path]. The viewer lazily loads **one** asset — the item under the playhead — and reloads on a path change (Story 4.3), so NFR-P6 (≥10 items) holds with VRAM at ~one asset regardless of item count. There is **no** shared cache, so the same `.glb` placed on two tracks would be re-loaded each time the playhead crosses from one to the other (a per-path **cold reload**, the Story 4.3 cost — bounded by the path-change gate, not by item count). **Trigger:** if rapid scrubbing across many distinct-then-repeated items makes per-boundary load time an issue, add the shared `Asset` cache (the D2 factory pattern) keyed by file path (architecture.md:220/1101 — post-MVP). *(Take `D_PLAYRATE` is **already** logged from 4.4 above — not duplicated here; the priority walk does not change that deferral.)*

---

## Deferred from: story 4.4 (native-media coexistence) — 2026-06-27

- **Take playrate (`D_PLAYRATE`) is not honored** [`src/pcm_source_anim.cpp` `GetCurrentAnimItem`]. Story 4.4 makes an animation item behave like native media for **move / resize / color / rename** and honors the take's `D_STARTOFFS` (left-trim → `animTime = (pos − itemStart) + D_STARTOFFS`), but a change to the take's **playrate** does **not** time-stretch the animation in this MVP — the displayed time advances at 1.0× regardless of `D_PLAYRATE`. **Trigger:** if sound designers need to retime an animation item via playrate, multiply the item-relative term by `D_PLAYRATE` in `GetCurrentAnimItem` (`animTime = D_STARTOFFS + (pos − itemStart) * D_PLAYRATE`, one more `GetMediaItemTakeInfo_Value` call) **and** reconcile it with Story 4.2's `GetLength` / `ProbeAnimationDuration` item-sizing, which currently assumes playrate 1.0 — changing a take's playrate makes Reaper auto-resize the item, so the displayed-time mapping and the item-length contract must move together. That cross-story coupling (4.2 ↔ 4.4) is why playrate is deferred while `D_STARTOFFS`, which has no such coupling (a left trim leaves item length unchanged), ships in 4.4.

---

## Deferred from: code review of story-3.3 (2026-06-26)

- **Palette uploaded without an `isfinite` screen** [`src/renderer.cpp` `RenderFrame`]. The per-frame `ComputePose` output is uploaded verbatim via `glUniformMatrix4fv`; inputs are guarded (3.1 weight `isfinite`, 3.2 tps-finite + quat-normalize) but a degenerate zero-scale/sheared bind bone could still compose a NaN into the palette → on-screen NaN-explosion vs AC7. **Trigger:** if a rig explodes/vanishes at the AR19 visual gate, add a cheap finite-screen (or log `palette_[0]` + AABB) as a §C diagnostic; a per-frame scan over ≤128 mat4 is the cost to weigh.
- **Clip-less / non-canonical skinned rig misplaced via the static fallback** [`src/renderer.cpp` + `src/asset_loader.cpp` §C]. When `pose_valid` is false (no clip / no-op), a skinned mesh draws through the static path at mesh-local space, which equals bind pose ONLY for canonical (identity-bind-palette) skins. A rig with a non-identity mesh→armature bind renders displaced/mis-scaled. **Trigger:** the AR19 in-Reaper visual gate — if the validated FBX comes out displaced, walk the §C knob order (`globalInverse` premultiply first for FBX). This is the dominant-risk gate item, not a Linux-side fix.

---

## Deferred from: code review of story-3.2 (2026-06-26)

- **`ComputePose` size-mismatch no-op has no diagnostic** [`src/animation.h`]. On a buffer/channel size mismatch, `ComputePose` returns without writing, so a caller's pre-zeroed palette/scratch read as a *valid-looking* all-zero pose (0 is finite, root tx (0,0,0)) — silently masking a wiring bug. Harmless in 3.2 (the only caller, `DumpAnimation`, always pre-sizes to `bones.size()`). **Trigger:** Story 3.3, when a live per-frame caller (`Animator`/`PoseBuffer` sized at `SetAsset`) is wired into the render loop — add an assert/once-warn on mismatch so a mis-sized buffer is loud, not a frozen rig.

---

## Architectural decision recorded 2026-05-10 (PRD step-04 round)

**Decision: viewer is rendered inside a ReaImGui dockable panel, not a standalone Win32 window.**

The Phase 0 standalone Win32 viewer window (`viewer_window.cpp`) is superseded by a ReaImGui-driven panel that docks into Reaper's docker system. This was raised by Antho during PRD User Journey discovery (step-04) — *"j'aimerai qu'il soit embedded dans une fenetre ReaImGui comme ca on peut Docker la fenetre dans l'interface Reaper. C'est important."*

**Consequences:**

- A new **Phase 0.5** is inserted in the build plan (~1.5–2 days, slot in Week 2): refactor `src/viewer_window.cpp` into `src/imgui_panel.cpp`. Keep WGL context creation (sokol_gfx still needs a GL context) and the action registration. Remove `RegisterClassExW`, `CreateWindowExW`, `WindowProc` — ReaImGui replaces all of that.
- Rendering pattern: sokol_gfx renders the 3D scene to an off-screen FBO; the resulting texture handle is passed to `ImGui::Image()`. ReaImGui handles panel chrome, docking, resize, and input.
- ReaImGui (cfillion's extension, ReaPack `cfillion/reaimgui`) becomes a runtime dependency. The ReaPack manifest must declare it as an auto-install dependency.
- Cross-platform window management is now ReaImGui's problem, not ours. The original Phase 0 SWELL-port plan (deferred from `extern/WDL`) is rendered moot — ReaImGui internally uses SWELL on Mac/Linux.
- Three earlier deferred items become **superseded** (see annotations in the Phase 0 review section below).

**Triggered by:** PRD step-04 architectural input.
**Pick up:** Phase 0.5 must precede Phase 1 (since Phase 1 renders into the panel).

---

## From Phase 0 review (2026-05-09)

### Build hardening

- **/Zc:__cplusplus flag** — without it, MSVC reports `__cplusplus = 199711L`
  even with `CXX_STANDARD 17`. No vendored SDK header currently guards on
  `__cplusplus >= 201703L`, so this is dormant. Pick up **when adding any
  third-party header that uses `if __cplusplus >=` guards** (likely Phase 1
  with assimp/glm/sokol_gfx).

- **/WX warnings-as-errors** — current build sets `/W3` but not `/WX`, so
  warnings still produce a successful build. AC1 requires "zero warnings"
  but enforcement relies on the validator manually inspecting the build
  log. Pick up **when CI is added** (Phase 5) or earlier if a regression
  ever sneaks a warning past the gate.

- **CMAKE_SOURCE_DIR → PROJECT_SOURCE_DIR** in `cmake/ReaperPlugin.cmake`
  for the SDK include path. Phase 0 doesn't compose, but if this project
  ever becomes an `add_subdirectory()` of a larger CMake build the SDK
  path will resolve to the parent's source dir. Pick up **if/when this
  project is consumed as a subdirectory**.

### Window UX polish

- **~~`hIcon` and `hIconSm` on the window class~~** — **SUPERSEDED 2026-05-10 by ReaImGui decision**: the window class no longer exists in Phase 0.5+; ImGui panels don't carry HWND-level icons.

- **~~`SetForegroundWindow` Win10/11 rate-limiting~~** — **SUPERSEDED 2026-05-10 by ReaImGui decision**: foreground/focus handling now goes through Reaper's docker; the OS rate-limit issue doesn't apply to docked panels.

### Pixel format leanness

- **`pfd.cDepthBits = 24; pfd.cStencilBits = 8`** — Phase 0 only clears
  the framebuffer, never tests depth or stencil. Currently we request
  depth/stencil pessimistically. Some integrated GPUs may not match a
  24/8 format and fall back via `ChoosePixelFormat`. Pick up **at the
  start of Phase 1 (mesh rendering)** when actual depth-test policy is
  decided — the pixel format choice depends on whether we go forward-
  rendered with z-buffer or deferred.

### Reaper API loader extension

- **Capture more API functions** as later phases need them: `Main_OnCommand`,
  `GetPlayPosition2Ex`, `GetSetMediaTrackInfo_Value`, `EnumProjects`,
  project state hooks for camera persistence, etc. Add `REAPERAPI_WANT_*`
  defines in `src/reaper_api.h` as needed. Pick up **incrementally per
  phase**.

### Modeless dialog wiring

- **~~`plugin_register("accelerator", ...)`~~** — **SUPERSEDED 2026-05-10 by ReaImGui decision**: ReaImGui handles keyboard input routing through its own accelerator hooks; no manual `plugin_register("accelerator", ...)` needed from us.

---

## Deferred from: code review of story-1.4 (2026-06-24)

- **DC leak on a stale `g_hwnd`** ([src/viewer_window.cpp:96](../../src/viewer_window.cpp#L96)) — `CloseViewerWindow`'s `!IsWindow(g_hwnd)` branch routes through `DestroyGLContext`, where `WindowFromDC(g_hdc)` returns null for an already-destroyed window, so `ReleaseDC(null, g_hdc)` silently fails and the DC leaks. Pre-existing (the `WindowFromDC` fallback shipped in 1.3/HEAD, owned by 1.4 per the story). Narrow trigger: Reaper would have to destroy our child window out from under us, and it only happens at unload, where the single-DC leak is harmless. **Pick up if** a future story sees Reaper externally destroying the docked HWND (e.g. screenset/layout changes), or if a leak audit flags it.

---

## Deferred from: code review of story-2.3 (2026-06-24)

- **Textured colored metal derives a white specular highlight** ([src/asset_loader.cpp:100-103](../../src/asset_loader.cpp#L100-L103)) — `ConvertMaterial` tints the specular toward `baseColorFactor`, but a gold/copper metal commonly authors `baseColorFactor=(1,1,1)` with the actual color in the diffuse *texture*. The loader sees only the factor (texture binding is a separate step), so the derived specular is white instead of base-color-tinted — a fidelity gap on exactly the metallic-roughness case FR16 targets. Correct handling needs the albedo texel, i.e. texture-aware / true PBR shading (a metalness uniform threaded into the shader, applied post-texture). That is explicitly out of Story 2.3's scope (Dev Notes §C: Blinn-Phong approximation only, no PBR BRDF). **Pick up as** optional Epic-6 polish if the gate finds metallic highlights read wrong, alongside the deferred metal-diffuse-suppression knob (§C).

---

## Deferred from: code review of story-3.1 (2026-06-26)

- **Same-named (or empty-named) distinct joints collapse to one global index** ([src/asset_loader.cpp](../../src/asset_loader.cpp) `DfsIndexJoints`) — the D1 skeleton keys bones by name (§B mandates dedup-by-name, the assimp bone↔node matching is name-based), so two distinct nodes sharing a name (common in mis-authored FBX left/right chains, or assimp's unnamed intermediate nodes → empty name) collapse to a single index. The second subtree's weights silently scatter onto the first bone and its `parentIdx` links point at the wrong parent, with no warning. Truly fixing it requires an identity model beyond name-matching (e.g. node-path keys) — out of 3.1's name-keyed model. **Pick up** in 3.3 if a real Demute fixture exhibits duplicate joint names; a cheap interim mitigation is a warn-once when `DfsIndexJoints` sees a name already indexed.
- **No >128-bone palette cap** ([src/asset_loader.cpp](../../src/asset_loader.cpp) `BuildSkeleton`) — `boneIds` can exceed the D13 std140 128-mat4 palette ceiling for a large rig, silently overflowing 3.3's GPU palette. Spec §F explicitly defers the `kMaxBones`/cap decision to 3.3 (validated Mixamo fixture = 99 bones, within ceiling). **Pick up** in 3.3 when the skinning shader + palette UBO are built — clamp/warn at parse or size the palette to the rig.

---

## Deferred from: code review of story-6.5.3 (2026-06-28)

- **`tools/gen_icons.py` is fragile on malformed SVG input** ([tools/gen_icons.py](tools/gen_icons.py)) — the offline SVG→RGBA rasterizer that generates `src/overlay_icons.h` crashes with bare `AttributeError`s on inputs the three checked-in icons happen to avoid: an empty / non-`<path>` SVG → `re.search(r'd="([^"]+)"', svg).group(1)` on `None`; only the **first** `d="..."` is captured, so a multi-`<path>` icon rasterizes incompletely with no warning; a `d` string starting with a number (relative-first or malformed) → `cmd.islower()` on `None`; an all-collinear/zero-height path → empty edge list → silently blank (transparent) icon. Build-time tooling only — not shipped in the DLL, and `icon_menu/light/color.svg` all parse cleanly today. **Pick up if** a contributor swaps in a multi-path or non-path icon (Material-style) and gets a cryptic stack trace or a blank glyph — add a clear error + multi-path concat + a leading-command guard.
