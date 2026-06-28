# Phase 4.5 — Validator Gate

Step-by-step acceptance test for **Phase 4.5** (Epic 6.5 — **viewport visual fidelity &
on-canvas tools**: the render should look like the source where it was downloaded —
Mixamo parity — plus the on-canvas tools that follow). Written for Antho (the validator)
to run on Windows. This file is the **sole authority on Phase 4.5 completion** (AR19) and
grows as Epic 6.5 progresses: **Story 6.5.1** seeds the rows below (source-fidelity
rendering — sRGB + balanced lighting + dielectric specular + normal maps); Stories 6.5.2
(silence console logs), 6.5.3 (tool sidebar + light tool), 6.5.4 (floor tool), and 6.5.5
(render-quality toggles + on-canvas FPS) append their own.

**You are testing the right thing if**, for Story 6.5.1, you can **load a Mixamo
character and a textured glTF/GLB next to its source render** (Mixamo's own preview, or
Blender / an online glTF viewer) and judge that the **colours and brightness are
acceptably close** — no longer washed-out/desaturated, no faces crushed to black, skin
and cloth reading **matte rather than plastic**, and surface detail restored where a
**normal map** is present — while **every Epic 2 static mesh and Epic 3 skinned rig still
renders and animates** at **≥60 fps**.

This is a **visual** gate: the Linux dev box compiles the non-`_WIN32` units but **cannot
see the render**, so the GLSL + vertex layout were self-reviewed and the judgement is
yours in-Reaper on Windows.

---

## Prerequisites

- Reaper 7.x (`caller_version == 0x20E`, current SDK targets 7.72)
- Visual Studio 2022 with **Desktop development with C++**
- CMake ≥ 3.20 and Git for Windows on PATH
- The same assimp 6.0.5 + GLM 1.0.3 + stb vendored stack as Phases 1–4 — **no new
  dependency** in this story (no CMake change; `aiProcess_CalcTangentSpace` is a flag and
  the new GL enums are `#define`s in `gl_loader.h`). First configure is slow; later
  builds reuse the cache.

> Story 6.5.1 touches **only the GL-boundary files**: `src/scene.h`, `src/gl_loader.h`,
> `src/asset_loader.cpp`, `src/renderer.h`, `src/renderer.cpp`. **No** change to
> `viewer_window.cpp` (the WGL/pixel-format context is left untouched — the sRGB output
> is done **shader-side** precisely so the Spike-0-proven context is not disturbed),
> `plugin_main.cpp`, `reaper_api.*`, `console_log.*`, `pcm_source_anim.*`, or
> `CMakeLists.txt`. The boundary rule (AR15) is preserved — no `rec->Register`, no new
> `REAPERAPI_WANT_*`.

## 1. Build & install

Identical to the Phase 1–4 gate (§1):

```cmd
build.bat
```

(one-click build + install into the Reaper `UserPlugins` folder; closes Reaper first,
copies the DLL, prompts to relaunch — see `docs/PHASE1_VALIDATOR_GATE.md` §1 if you need
the manual steps). Build must be clean at `/W3 /permissive-`.

## 2. What you are testing

Story 6.5.1 fixes the render so it matches the source in **four independent ways**, all
visible in a Mixamo side-by-side:

1. **sRGB-correct colour** — the base-colour texture is uploaded `GL_SRGB8_ALPHA8` (the
   GPU decodes sRGB→linear on sample) and the final fragment is encoded linear→sRGB on
   write. The render is no longer washed-out / desaturated. *(Mechanism note: the
   architecture names `GL_FRAMEBUFFER_SRGB`, but the default framebuffer is a legacy
   non-sRGB pixel format and the renderer draws straight to framebuffer 0, so the
   **shader-side encode** is the authoritative path and `GL_FRAMEBUFFER_SRGB` is **not**
   enabled — enabling both would double-encode and over-brighten.)*
2. **Balanced lighting** — a face turned away from the key light keeps a readable
   ambient/fill term instead of crushing to near-black. Light colour, direction, and the
   ambient amount are driven by uniforms (`u_lightColor` / `u_lightDir` / `u_ambient`),
   set to sensible defaults now and consumable by the **light tool (Story 6.5.3)**.
3. **Dielectric-correct specular** — a non-metal (skin, cloth) reads **matte**, not
   plastic/metallic; a genuine metal still reads glossier (FR16 distinction preserved).
4. **Normal maps** — an asset that carries a normal map shows restored surface detail
   (the tangent frame is built **per-fragment from screen-space derivatives** — no vertex
   tangent; the map uploaded **linear**); an asset **without** one (or without usable UVs)
   renders **unchanged** — the feature is gated on presence.

## 3. Story 6.5.1 — source-fidelity rendering (sRGB + lighting + dielectric specular + normal maps)

**Suggested fixtures:**
- *Mixamo character* with a base-colour texture (e.g. the same *Hip Hop Dancing.fbx* /
  a downloaded Mixamo character `.fbx`/`.glb`) — ideally one that also ships a **normal
  map** for row 4.
- *A textured glTF/GLB* you can also open in Blender or an online glTF viewer for a
  side-by-side colour reference.
- *A static `.glb`* (Epic 2) with no animation.
- *A skinned rig* (Epic 3) that animates with the transport.

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | sRGB colour parity | Load the textured Mixamo/glTF character and place it next to its **source render** (Mixamo preview / Blender / a glTF viewer). Colours and brightness are **acceptably close** — saturated where the source is saturated, **not washed-out / milky / grey**. | AC1 |
| 2 | Faces away from the key light stay readable | Orbit so a surface turns **away** from the light. It **dims but stays readable** (you can still make out its colour/detail) — it does **not** crush to near-black. | AC2 |
| 3 | Skin/cloth reads matte, not plastic | On a character's **skin or cloth**, there is **no broad blown-out plastic/metal sheen** — the highlight is small and dim (matte dielectric). A genuinely **metallic** material (if present) still looks **glossier** than the skin. | AC3 |
| 4 | Normal map restores detail; absent map unchanged | An asset **with** a normal map shows **restored surface detail** (wrinkles/pores/stitching catch the light as you orbit). An asset **without** a normal map (or without UVs) renders **exactly as before** — no artefacts, no black/!inverted surfaces. | AC4 |
| 5 | No regression — static + skinned + perf | An **Epic-2 static mesh** still renders correctly (geometry identical; colour changes by design). An **Epic-3 skinned rig** still **deforms, loops, and holds the end frame** under the transport (Epic 4). At the **10+-item fixture** the viewport holds **≥60 fps** — no stutter introduced by the extra texture sample / TBN math. | AC5 |

> **Scope note (do NOT fail 6.5.1 for these):** there is **no tool sidebar / light-tool
> UI** yet (that is **Story 6.5.3** — this story only introduces the light uniforms with
> hardcoded defaults); **console logs are still present** (removing them is **Story
> 6.5.2**, incl. the FPS log); there is **no floor/grid** (6.5.4) and **no render-quality
> toggles or on-canvas FPS readout** (6.5.5); this is **not** full PBR / metallic-roughness
> BRDF / IBL / shadows / tone-mapping (Blinn-Phong stays — post-MVP); **no MSAA**. If the
> four colour/lighting/specular/normal-map checks read acceptably and nothing from Epics
> 2–4 regressed at ≥60 fps, 6.5.1 passes.

> **Tuning at the gate:** the lighting balance (`u_ambient` default ≈ 0.35), the key-light
> direction/colour, and the specular strength (`kSpecStrength` ≈ 0.35 in the fragment
> shader) are **numbers tuned by eye against this gate**. If a face is still too dark, or
> skin still too shiny, or the overall image too bright/dim, those three knobs are the
> first dials — say which way it's off and they get nudged (they live in `renderer.cpp` /
> `renderer.h` and are a one-line change each).

## 4. Recording the result

**Result:** Story 6.5.1 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-28).
A textured FBX character (Mixamo "Catwalk") **and** a textured glTF/GLB (steampunk
explorer) both render with **correctly-placed, detailed textures**, sRGB-correct colour,
balanced lighting, matte dielectric skin, and normal-map surface detail; no Epic-2/3
regression. The in-Reaper visual judgement IS the gate (AR19) — the Linux dev box compiles
the non-`_WIN32` units but cannot see the render.

### Gate-iteration findings (the render bugs this gate caught — all root-caused, not guessed)

The visual gate surfaced two real bugs that the dev side then reproduced **offline** (by
loading the exact model files with assimp + the embedded textures on the Linux box) before
fixing — turning "it looks wrong" into a measured root cause:

1. **UV V-flip for ALL formats (the "smeared / offset texture" bug).** assimp delivers UVs
   in a **bottom-up** convention for *every* importer (it flips glTF's top-left origin to
   match FBX/OBJ/Collada), while we upload textures **top-down** (stb). So V must be flipped
   for **all** formats (`aiProcess_FlipUVs`) — measured directly: assimp's glTF V = `1 −`
   the raw-glTF V. Without it, a UV atlas (a character body) samples the **wrong/padded**
   texture region → "smeared marble" on FBX, "offset texture" on GLB. An interim build that
   flipped only non-glTF left GLB V-flipped; the final rule is **always flip**.
2. **Energy-normalized specular (the "washed-out metal" bug).** A glTF metal (metallic≈1,
   roughness≈1 → shininess floored at ~2) gave a huge bright Blinn-Phong lobe that **washed
   the whole surface to pale grey**. Fixed with the `(n+8)/(8π)` Blinn-Phong normalization
   so a broad lobe is dim and a tight lobe bright (dielectrics are visually unchanged).

Also changed during the gate vs the original task spec: normal mapping uses a **per-fragment
tangent frame from screen-space derivatives** (robust to mirrored UVs — a per-vertex tangent
cancels to zero along a character's mirror seam and silently dropped the normal map on the
torso), so **no vertex tangent attribute** is uploaded and `aiProcess_CalcTangentSpace` is
not used; the authored Phong specular is **always ignored** in favour of a metalness-derived
dielectric value (FBX/Mixamo author a 0.5 grey specular that reads plastic); and the
`aiTextureType_HEIGHT` normal-map fallback was dropped (it is a grayscale bump map, not a
tangent-space normal). See the architecture AR20 Spec Change Log entry.

**Deferred (NOT 6.5.1):** **alpha/transparency** — the steampunk GLB has two `BLEND`
materials (a window at opacity 0.11, a bulb at 0.5); we render everything opaque, so those
parts show as solid panels. Alpha blending is its own feature/story (recorded in
`deferred-work.md`).

---

## 5. Story 6.5.2 — silence all console logging by default

Story 6.5.2 makes the viewer **silent in the console by default**. Every `[RAV]` line in
the codebase is produced by one funnel (`Emit()` in `src/console_log.cpp`); its body is now
compiled out unless `RAV_ENABLE_CONSOLE_LOG` is defined, so a **normal build prints nothing**
— all ~43 `Log{Info,Warn,Error}` call-sites go quiet at once, none are deleted. The
diagnostic capability is preserved for a debug build (`build_debuglog.bat`).

This is a **console** gate, observable **only in Reaper on Windows**: the Linux dev box
compiles the unit but cannot open Reaper's console. Open Reaper's console with **Extensions ▸
ReaScript console / `View ▸ Show console output`** (Reaper's "Show console output").

> **Scope note (do NOT fail 6.5.2 for these):** the on-canvas **FPS readout** and the
> **on-canvas load-failure indication** are **NOT built here** — they land in **Story 6.5.5**
> (they need the on-canvas text-overlay mechanism introduced with the sidebar in 6.5.3/6.5.5,
> which does not exist yet). 6.5.2 only **silences** the console and **records** the rehome
> (see `deferred-work.md`). Between 6.5.2 and 6.5.5 a load failure has **no** visible signal —
> this is a **dev-time-only** window; 6.5.5 lands before ship (Epic 7).

**Suggested fixtures:** a project with **2–3 animation items** (FBX + GLB), plus **one item
deliberately pointing at a missing/renamed file** (to exercise the silent failure path).

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Normal build is silent across load/play/save | Run **`build.bat`** (normal build). Open the viewer and Reaper's console, then **load** the items, **play** (move the playhead so the rig animates), and **save → close → reopen** the project. The console shows **no `[RAV]` lines at all** — including no FPS line and no load line. | AC1 |
| 2 | Even the failure path is silent | Include the item with the **missing/broken file**. Loading/scrubbing onto it prints **no `[RAV]` error** in the console (the on-canvas load-failure indicator is deferred to 6.5.5 — its absence here is expected, not a failure). | AC1 |
| 3 | *(Optional — proves AC2)* Debug build brings logs back | Run **`build_debuglog.bat`**, repeat the load/play/save run → the **`[RAV] <level>: <message>` lines reappear** in the prior format (FPS line, load lines, etc.). Then **re-run `build.bat`** to return to the shipping silent build. | AC2 |

**Result:** Story 6.5.2 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-28).
With only the current `reaper_animviewer.dll` loaded, Reaper's console stays clean at launch
and across load → play → save/reopen — no `[RAV]` lines, no FPS line, no load-failure line.
The in-Reaper console observation IS the gate (AR19) — the Linux dev box compiles
`console_log.cpp` both ways (silent default + `-D RAV_ENABLE_CONSOLE_LOG`) but cannot open
Reaper's console.

_Gate note:_ the initial run still showed a `[FBXAV] extension loaded (Phase 0)` flash at
launch. Root cause was **not** a 6.5.2 code defect — a **stale pre-rename DLL**
(`reaper_fbxanimationviewer.dll`, 2026-05-09) was still present in
`%APPDATA%\REAPER\UserPlugins\` and loaded alongside the current `reaper_animviewer.dll`; the
legacy `[FBXAV]` logger lived only in that old binary. Deleting the stale DLL (current code uses
the `[RAV]` prefix and routes every log through the now-silent `Emit()`) made the console silent.
Lesson: a project rename changes the DLL name, so old-named binaries linger in UserPlugins and
get co-loaded — purge them after a rename.

---

## 6. Story 6.5.3 — viewport tool UI + light tool

Story 6.5.3 adds an **in-viewport tool UI** and its **first tool, a light control**.

> **Final design (Antho feedback during review):** earlier cuts (native Win32 child buttons → a
> hand-drawn GL overlay → a plain ImGui window) flickered or looked amateur. The UI is now built with
> **Dear ImGui** (the library behind ReaImGui) **vendored into our plugin** and rendered into our GL
> context as a true overlay — **statically linked, NOT the ReaImGui runtime extension**, so FR49's "no
> ReaImGui dependency" still holds (single self-contained DLL, no ReaPack install). It is a **frameless
> hamburger menu** that **shows/hides a list of tools**, using **Antho's icons** (`Icons/icon_*.svg`,
> rasterized to textures). The old floating **Reset View** is gone; camera recenter (FR25) is a menu
> item.

Clicking the **hamburger** (≡, top-left) opens a frameless panel with: a **Recenter camera** button;
a **Light** section (bulb icon) with a **Colour** picker (palette icon + ImGui swatch/popup) and a
**Position** control — a **circular pad** that is the *sphere around the model laid flat*: centre =
light straight overhead, the mid-ring = horizon, the rim = straight below, the angle = which way
around. **Drag the dot** to place the light in space. Colour + pad **live-update** the render via
Story 6.5.1's `u_lightColor` / `u_lightDir` (FR50); the shader and mesh `RenderFrame` path are
unchanged (ImGui draws after the scene).

This is a **GL/Win32 UI** gate, observable **only in Reaper on Windows**: the Linux dev box does not
compile the `_WIN32` units (or build the ImGui lib), so the integration is **self-reviewed** (ImGui
API verified against the pinned v1.91.5 release; icons rasterized + visually checked offline) and the
judgement is Antho's, in-Reaper (AR19).

> **Scope note (do NOT fail 6.5.3 for these):** the **floor + grid** tool is **Story 6.5.4** (FR51)
> and the **render-quality toggles + on-canvas FPS readout** are **Story 6.5.5** (FR52/FR53) — so the
> menu has **only** the recenter + light tools for now; that is expected. **No** `u_ambient` slider and
> **no** persistence of the light across save/reopen (session-only, like the camera) are in scope. If
> an icon looks upside-down, that is a one-line UV flip — note it, it is not a failure.

**Suggested fixtures:** any project with **1–2 animation items** (FBX + GLB) on a track, played under
the playhead so the model is lit and visible. Resize / undock the docker.

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Hamburger appears, clean, no flicker | Open the viewer. A small **≡ hamburger** (Antho's menu icon) sits at the **top-left** over the render, **steady — no flicker** including on mouse-over. **Resize / undock / re-dock** → it stays pinned top-left. | AC1 |
| 2 | Menu show/hides | **Click the hamburger** → a frameless panel opens listing the tools (Recenter, Light colour, Position pad), with the **bulb** and **palette** icons. Click the hamburger again → it **collapses** back to just the icon. | AC1 |
| 3 | Recenter works; camera intact | Orbit/zoom away, click **"Recenter camera"** → the model re-frames. **Right-drag orbit / wheel zoom / middle-drag pan** still work, **and** interacting **over the menu** drives the widgets, not the camera (no fighting). | AC1/AC3 |
| 4 | Colour picker changes the light live | Click the **Colour** swatch → ImGui's picker opens **inside the view** (no OS dialog, no freeze). Pick e.g. red → the model is lit red live and the swatch updates. | AC2 |
| 5 | Position pad moves the light live | **Drag the dot** on the circular pad → the lit/shadowed sides sweep as the light orbits; **centre = overhead**, **rim = below**, **angle = direction**. Smooth, live, intuitive. | AC2 |
| 6 | Clean reopen, no leak/ghost; fps | **Close** the viewer (toggle action) and **reopen** → the menu is back and working, no ghosts. Skinned/static models still render and hold **≥60 fps** with the menu open and during pad drags. No `imgui.ini` appears next to Reaper; console stays silent (6.5.2). | AC3/AC4 |

**Result:** Story 6.5.3 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-28: "C'est parfait").
The vendored Dear ImGui tool UI — a frameless top-left **hamburger menu** (Antho's icons) that
show/hides Recenter camera + light **Colour** picker + the **circular position pad** (sphere laid
flat) — renders cleanly on top of the 3D view with **no flicker**, the colour and the draggable light
pad update the render live, and camera orbit/zoom/pan coexist with the UI. Statically linked, single
self-contained DLL (no ReaImGui runtime dependency). The in-Reaper visual/interaction judgement IS the
gate (AR19) — the Linux dev box stubs the Windows target and cannot build the `_WIN32` units or the
ImGui lib (the integration was self-reviewed against the pinned ImGui v1.91.5 and the icons verified
offline).

---

## 7. Story 6.5.4 — always-on floor + shadow-quality tool (Off / Low / Mid / High)

Story 6.5.4 puts a **ground plane + grid** under the model (always on — there is no toggle, it is
simply part of the scene) and a real **cast shadow** from the model onto that floor, with a **4-level
quality selector** (Off / Low / Mid / High) in the 6.5.3 tool menu so a weaker PC can dial the shadow
down or off. The floor is a flat unlit plane that *receives* the shadow; the shadow is a real-time
shadow-mapping pass (a depth render from the light's POV + PCF sampling on the floor), rebuilt each
frame from the **current light direction** so the 6.5.3 light pad moves the shadow too.

This is the **heaviest rendering story of Epic 6.5** — it builds a shadow-mapping pipeline and (with
Antho's direction, logged in the AR20 Spec Change Log) it makes the renderer bind a **transient
offscreen depth FBO** for the depth pass, a sanctioned deviation from the original "renderer never
binds an offscreen FBO" rule (the FBO is bound only for that one pass; the visible output still goes to
framebuffer 0). It is a **visual + perf** gate, observable **only in Reaper on Windows**: the Linux dev
box compiles the non-`_WIN32` units but cannot build the renderer/ImGui units or see the render, so the
GL FBO + PCF usage was **self-reviewed** (standard shadow-mapping mirrored from the proven mesh path;
the ImGui radios mirror the proven 6.5.3 widgets) and the judgement is Antho's, in-Reaper (AR19).

> **Scope note (do NOT fail 6.5.4 for these):** the **render-quality toggles** (normal maps / MSAA)
> and the **on-canvas FPS readout** are **Story 6.5.5** (FR52/FR53) — only the **shadow** quality lever
> lands here. **Minor aliasing** on the grid lines, or **slight shadow acne / softness** at a given
> level, are **tuning notes** (the bias / `kShadowFloor` / floor greys / shadow-map sizes are in-code
> constants tuned by eye at this gate), not failures. The **floor and the shadow quality are not
> persisted** (session-only, like the camera and the 6.5.3 light) — a fresh viewer opens at the default
> level (Mid). **Model self-shadowing is OPTIONAL** and was **deferred** to keep the gate-validated
> mesh shaders byte-for-byte unchanged — the **required receiver is the floor**, so the model's own
> surface not self-shadowing is **expected, not a failure**. A **lit/textured/reflective** floor is out
> of scope (flat grey by design).

**Suggested fixtures:**
- *A skinned rig* (Epic 3 / Mixamo) that animates under the transport — to watch the shadow track the
  pose and the moving limbs.
- *A static `.glb`* (Epic 2) — to confirm the floor + a still shadow.
- The **10+-item** project (NFR-P1) — to confirm **Off and Low** hold **≥60 fps** during playback.
- Resize / undock / re-dock the panel.

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Floor is always under the model | Load any model. A **solid ground plane + grid** sits at the model's **feet**, centred under it and sized to it. The **empty idle panel** (no asset) shows **no floor** (unchanged from before). | AC1 |
| 2 | Shadows control with 4 levels | Open the **hamburger menu** → a **Shadows** section (Antho's shadow icon) shows **Off / Low / Mid / High**. The current level is reflected; clicking another takes effect **immediately** (next frame). | AC2 |
| 3 | Clean cast shadow at Mid | At **Mid**, the model casts a **clean soft shadow** on the floor — **no strobing, no acne stripes, no peter-panning** (shadow detached from the feet). | AC2/AC3 |
| 4 | Light pad sweeps the shadow | Open the **light Position pad** and drag the dot → the **shadow sweeps live** as the light orbits (the shadow is cast from the current light direction). | AC3 |
| 5 | Low harder, High softer, Off none | **Low** looks **harder/sharper**, **High** looks **softer**; **Off** removes the shadow entirely **and the floor still draws**. | AC2/AC4 |
| 6 | Camera + tools still work | **Right-drag orbit / wheel zoom / middle-drag pan**, **Recenter**, and the **light colour/position** tools all still work with the floor + shadow present; no fighting between the menu and the camera. | AC3 |
| 7 | Perf + clean reopen | At **Off and Low**, playback holds **≥60 fps** at the 10+-item fixture (Mid/High may cost more by design). **Close → reopen** the viewer (toggle action) → the floor + shadows are back at the **default (Mid)** level, **no ghost/leak**, console stays **silent** (6.5.2). | AC4/AC6 |

> **Tuning at the gate:** the shadow **bias** (`≈0.0015`), the floor **shadow darkening** (`kShadowFloor
> ≈ 0.45`), the floor **greys** (solid `≈0.30` / grid `≈0.42`), the floor **size** (`6× frameRadius`),
> and the **shadow-map sizes** (512 / 1024 / 2048) are in-code constants in `renderer.cpp` /
> `renderer.h`. If the shadow has acne, or is too dark/light, or the floor is too small/large, say which
> way it is off — each is a one-line nudge.

**Result:** Story 6.5.4 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-28). The always-on
floor + grid sits under the model and the model casts a real shadow onto it; the **Shadows** tool's
Off / Low / Mid / High selector takes effect live, the light pad sweeps the shadow, and **Off** removes
it while the floor stays. The in-Reaper visual + perf judgement IS the gate (AR19) — the Linux dev box
compiled the non-`_WIN32` units and syntax-checked the renderer but could not build the `_WIN32`
renderer/ImGui units or see the render, so the floor + shadow-mapping pipeline was self-reviewed and
Antho judged it in-Reaper.
