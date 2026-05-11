---
stepsCompleted: ['step-01-init', 'step-02-discovery', 'step-01b-continue', 'step-02b-vision', 'step-02c-executive-summary', 'step-03-success', 'step-04-journeys', 'step-05-domain', 'step-06-innovation', 'step-07-project-type', 'step-08-scoping', 'step-09-functional', 'step-10-nonfunctional', 'step-11-polish', 'step-12-complete']
completedAt: '2026-05-10'
releaseMode: 'phased'
inputDocuments:
  - '_bmad-output/planning-artifacts/prfaq-FBXAnimationViewer.md'
workflowType: 'prd'
classification:
  projectType: 'desktop_app'
  projectSubType: 'DAW extension / native plugin (Reaper)'
  domain: 'general'
  domainSubContext: 'creative production tooling / game audio'
  complexity: 'medium'
  projectContext: 'brownfield'
---

# Product Requirements Document - FBXAnimationViewer

**Author:** Antho
**Date:** 2026-05-10

## Executive Summary

FBXAnimationViewer is a Reaper extension that lets sound designers score 3D animations live inside their DAW, replacing the daily engine-capture roundtrip with a native, scrubable preview synchronized to Reaper's transport.

**Vision:** *Score 3D animation live in Reaper — load the glTF, scrub the transport, and skip the engine-capture roundtrip entirely.*

**Primary user:** Sound designers at Demute Studio (game audio outsourcing boutique, Mons, BE, ~10 staff, 8 years, 16 ReaPack tools already shipped). Daily workflow today: open the client's Unity/Unreal/Godot project, find the animation, record a video of the animation plus timeline, import the video onto a Reaper track, and design audio against the flat capture. Every animator iteration triggers a full recapture and realignment of existing sound work.

**Secondary user:** The broader game-audio community via ReaPack. Distributed as-is under MIT license; no support SLA. Adoption is opportunistic, not a primary product driver.

**Problem solved:** Two daily pains, both invisible inside a flat video capture:

1. *Temporal drift* — animator re-times a frame; sound stems need realignment. Currently: re-record, re-import, re-align.
2. *Visual / identity change* — animator reskins (e.g. armor leather → steel; character gets a wooden leg). Same timings, different sounds needed. Currently invisible until the next engine build.

The viewer collapses both pains into the same Reaper session: the rig is rendered in its current state, scrubbed by the host transport, and re-loadable when the source animation changes.

### What Makes This Special

**Two-dimensional review surface.** The viewer is not just a timeline-synchronized animation player — it is a *current-state* window that reveals both *when* things happen (temporal scrub) and *what they look like right now* (texture, material, sub-mesh identity). Tagline: *See what changed, hear what should change — directly in Reaper.*

**Lives where sound designers already work.** Existing alternatives (Autodesk FBX Review, F3D, MotionBuilder, Wwise 3D Viewer) require a context switch out of the DAW. FBXAnimationViewer attaches each animation as a native Reaper timeline item, and the rendered preview docks inside Reaper's own UI as a ReaImGui panel — no extra floating window cluttering the desktop, no context switch, zero monitoring re-setup.

**Whitespace verified.** The Cockos forum thread #192268 (2017) records this exact request, unanswered for eight years. No ReaPack package, no SWS extension, no GitHub project delivers DAW-transport-synchronized animation rig playback. Demute is uniquely positioned: the skill combination (game-audio insider + Reaper extension dev + 3D engine literacy) is rare enough that the gap persisted.

**Tight scope, defensible boundaries.** The viewer is a *read-only consumer* of the animation pipeline. It does not write back into DCC tools (Maya/Blender/MotionBuilder). It is not an authoring tool. Middleware integration (Wwise/FMOD) is explicitly out of scope — the viewer serves audio *production*, not *implementation*.

## Project Classification

| Field | Value |
|---|---|
| **Project type** | `desktop_app` — native DLL Reaper extension (Windows MVP, cross-platform architected) |
| **Domain** | `general` — creative production tooling, sub-context: game audio |
| **Complexity** | `medium` — native C++ in a hostile host process (Reaper), embedded 3D rendering (sokol_gfx + WGL), animation parsing and skinning (assimp + GPU vertex skinning), DAW transport integration. Not regulated, not trivial. |
| **Project context** | `brownfield` — Phase 0 (scaffolding) shipped and validated 2026-05-09 (7 of 8 acceptance checks green; the eighth is diagnostic). Phases 1–5 in plan: static mesh rendering with diffuse-textured Blinn-Phong → skinned animation → Reaper transport sync → file reload + camera persistence → polish & release. |
| **Distribution channel** | ReaPack repository (Demute Toolbox), free, MIT-licensed. |
| **Development model** | Solo: Claude Opus 4.7 = implementing developer, Antho (Demute founder, sound designer) = validator. Validator turnaround = critical path. |

## Success Criteria

### User Success

**Primary user (Antho, Demute internal).** Within two weeks of Phase 5 release, Antho replaces his daily engine-capture workflow with FBXAnimationViewer for ≥80% of sound-design sessions. "Replace" is binary per session: he does *not* open Unity/Unreal/Godot to record a video reference; the rig preview is sourced entirely from a `.glb`/`.gltf` loaded into Reaper.

**The "aha!" moment.** First time Antho scrubs the Reaper transport and the rig follows frame-accurate, scrubbed by item-relative time mapping, with both timing AND visual identity (texture, sub-mesh) visible in the viewer.

**Emotional outcome.** Sally's "preserved listening state" — the sound designer's ear stays calibrated on the sample loop while the eye verifies on-screen, with no context switch out of Reaper. Validation: Antho self-reports fewer interruptions and faster iteration on at least one real Demute project during Phase 5.

**Two-dimensional review verified.** Antho identifies a *visual reskin* (e.g. armor material change) on at least one real Demute project without opening the game engine, and swaps the corresponding sound stem inside Reaper — proving the viewer covers the visual dimension, not just temporal.

### Business Success

Demute is not selling this product, so business success is internal ROI + community goodwill — no revenue, no DAU, no churn.

**Internal ROI (Demute).**

- Antho documents ≥5 captures avoided per week within the first month of Phase 5 release. At ~20 min per capture, that is ≥1.7 hours/week of his time freed, ≥7 hours/month.
- 0 hours of Antho's time spent re-establishing the workaround (returning to engine-capture) during the same window.

**Community signal (ReaPack).**

- ReaPack listing live within 1 week of Phase 5 commit. Demute's existing 16 tools provide the distribution channel; no marketing push planned.
- ≥50 downloads in first 90 days (informal target — Demute's other tools historically reach 200–1000 downloads/year, but viewer audience is narrower).
- ≥0 critical issues requiring Antho's time as community support (distribution model is explicit no-SLA, so support load is the failure mode).

**Reputation halo (opportunistic, not committed).**

- 1 mention in A Sound Effect, gameaudio.slack, KVR, or a curated "best free Reaper tools" list within 6 months. Organic — no outreach.

### Technical Success

**Build & ship (Phase 0 baseline maintained).**

- Continues to compile with MSVC `/W3` zero warnings.
- Continues to install in <1 minute (drop DLL in `%APPDATA%\REAPER\UserPlugins\`).
- Continues to load in <2 seconds and produce no host-side errors at Reaper startup.

**Runtime (Phase 1–5 cumulative).**

- 0 host crashes during Demute internal usage from Phase 4 onward.
- Viewer renders at ≥60 fps on Antho's reference Windows machine with the heaviest Demute fixture (target: ~20k tris, 4 materials, 50-bone skeleton).
- Compatibility ≥80% on Demute's tested glTF/GLB fixtures, ≥60% on FBX fixtures (per Winston's Phase 2 risk assessment — FBX edge cases via assimp's parser).

**Reaper integration.**

- Animations appear as native Reaper timeline items via PCM_source plugin: drop a `.glb` onto a track, get a standard item with item-relative playhead mapping, exactly like dropping a video today.
- Viewer renders inside a **ReaImGui dockable panel**: ReaImGui declared as ReaPack dependency, auto-installed on extension install. No standalone top-level viewer window.
- Project state (camera angle, loaded animation per item, panel dock position) round-trips through Reaper project save/load with no manual reconfiguration.

### Measurable Outcomes

| Horizon | Metric | Target |
|---|---|---|
| Day 1 (Phase 5 commit) | Phase 5 validator gate passed | All check rows green |
| Week 1 | Reaper Action `FBXAV: Open Viewer` triggered by Antho on a real Demute project | ≥1 session |
| Week 2 | Engine-capture replacement | ≥50% of Antho's sound-design sessions |
| Month 1 | Captures avoided | ≥20 |
| Month 1 | Hours of Antho's time freed | ≥7 |
| Quarter 1 | ReaPack downloads | ≥50 |
| Quarter 1 | Antho support-time spent on community issues | <2 hours |

## Product Scope

### MVP — Minimum Viable Product (Phases 0–5, 6 weeks)

**Must work to be useful, must pass all phase-gate validations.**

- Reaper 7.x extension, Windows x64 (Phase 0 ✓ shipped — standalone Win32 viewer; replaced in Phase 0.5)
- **Phase 0.5 (~1.5–2 days, slot in Week 2)**: refactor viewer from Win32 top-level window to **ReaImGui dockable panel**. ReaImGui declared as ReaPack dependency. Render-to-FBO via sokol_gfx, presented to ImGui via `ImGui::Image()` texture handle.
- glTF / GLB primary format support, FBX secondary (via assimp)
- Static mesh rendering with diffuse texture + Blinn-Phong + per-material specular (Phase 1, Winston Option B)
- Skinned animation playback: bone hierarchy + matrix palette + GPU vertex skinning (Phase 2)
- Reaper transport sync via PCM_source plugin: drop .glb/.gltf/.fbx on a track → item with item-relative time mapping (Phase 3)
- Manual file reload via button; auto file-watcher downgraded to "P2 polish" (Phase 4)
- Camera orbit / zoom / pan, state persistence per Reaper project (Phase 4)
- One global viewer panel (dockable in any Reaper docker; the playhead-current animation is displayed); multi-panel deferred
- ReaPack manifest published with ReaImGui as auto-install dependency, MIT-licensed, distributed as-is no-SLA (Phase 5)

### Growth Features (Post-MVP)

- Per-track / multi-window viewers (instead of 1 global)
- Auto file-watcher (no manual reload click)
- Normal maps and richer material rendering (toward simplified PBR)
- Configurable lighting (HDRI, multi-light)
- Alembic format support (PRFAQ Phase 2 scope item, deferred to post-MVP)
- Mac and Linux ports (~25–30% additional effort per PRFAQ; community-demand driven)
- Engine-side export helpers (Unity/Unreal scripts that batch-export runtime animations to glTF/GLB)

### Vision (Future, not committed)

- Cinematic Sequencer composition import (Unreal Sequencer cuts → multi-track Reaper layout)
- Marker-style hints displayed on the rig surface (e.g. visual cue on the contact frame) — strictly read-only, no DCC roundtrip
- Visual diff mode between v_N and v_N+1 of the same animation source

## User Journeys

### Journey 1 — Antho's Tuesday: scoring an iterating animation (happy path)

**Persona.** Antho, sound designer at Demute Studio, mid-week, mid-project. He's been scoring impacts and foley for a dragon-slaying combo for two days. The Unreal project lives on the client's shared drive; Demute has read access. Antho's Reaper session is already loaded with twelve impact stems and a half-finished foley layer for the dragon's death fall.

**Opening scene.** It's Tuesday, 10:30. The animator pushed `dragon_slay_v11` overnight — third iteration this week. In Antho's pre-FBXAnimationViewer world, the next twenty-five minutes would be: tab out of Reaper, open Unreal, navigate to the asset browser, find the new animation, hit play, screen-record at 60fps, save to disk, copy back, drop the MP4 on Reaper video track 7, scrub to verify timing, realign every dependent sound stem. He's done this five times already this week.

**Rising action.** Instead, Antho exports `dragon_slay_v11.glb` straight from Unreal (he set up a one-click export macro last month). He drags the `.glb` from File Explorer onto track 3 in Reaper, right next to his impact stems. A new item appears — dark grey, named `dragon_slay_v11`, length matching the animation duration exactly. He hits the spacebar. The Reaper transport rolls; in the ReaImGui viewer panel docked at the bottom of Reaper's window, the dragon model swings the blade forward, the blade catches the air, the body begins its fall.

**Climax.** Frame 47 — where his `sword_leather_03.wav` sample landed perfectly in v10 — looks wrong somehow. He pauses the transport. The panel holds the frame. He rotates the camera slightly in the panel to see the impact point. *The dragon's armor changed.* From the leather scales of v10 to chunky steel plate. Same timing, but the sound is now a lie. Without leaving Reaper, without opening Unreal, Antho swaps `sword_leather_03.wav` for `sword_steel_02.wav` on the same track. He scrubs back to frame 45, plays through. Better. Right.

**Resolution.** Total elapsed: three minutes. He commits the Reaper project (camera state, item-bound animation references, and panel dock position serialize automatically), moves on to the next animation. The twenty-two minutes saved compound across five animations on this character — over a hundred minutes of context-switch he didn't pay today.

**Capabilities revealed.**

- Drop a `.glb` onto a Reaper track → PCM_source plugin creates a standard timeline item with length = animation duration.
- Playhead-relative time mapping: `animTime = playheadTime − itemStart`, clamped.
- Viewer renders inside a ReaImGui dockable panel (docks in any Reaper docker; no floating window).
- Visual fidelity sufficient to distinguish material types (Winston's Option B: diffuse texture + Blinn-Phong + per-material specular).
- Camera orbit/zoom inside the ImGui panel during playback without breaking transport.
- Project state (camera, item-animation binding, panel dock position) serializes through Reaper's project save.

### Journey 2 — Monday morning, new client project (edge case: rig variability)

**Persona.** Same Antho, Monday 09:15. New project from Studio X has landed overnight in the shared drive. First contact with Studio X's animation conventions.

**Opening scene.** 47 FBX files in the project folder: characters, weapons, ambient props, cinematic cuts. Antho opens the first one in a quick preview. Conventions are different from the last client: bones are named in French (`epaule_gauche`, `tete`), Z-axis is up instead of the glTF-canonical Y-up, units are centimeters instead of meters. None of this was communicated explicitly — he's discovering it by opening the files.

**Rising action.** Antho drops `villageois_run.fbx` onto track 1. The item appears, correct length. He hits play. The ReaImGui viewer panel renders the villager floating mid-air, oriented sideways, scaled at roughly 1/100th of what he expected — a tiny figurine running across an otherwise empty space. He frowns.

**Climax.** He clicks the **Reset Camera** button in the panel's toolbar. The camera reframes around the (sideways, tiny) rig. The animation is still playing, the panel hasn't gone blank or thrown an error overlay. He can see the character is wearing a green vest, holds a stick, has a satchel. The visual identity check works *despite the wrong orientation and scale*. Timing is still readable: the foot contacts are visible on the silhouette. He can score the foley.

**Resolution.** He posts in the team channel: *"Studio X uses Z-up, cm units, French bone names. Add to per-project axis remap feature request post-MVP."* He gets on with the day's work. The tool was useful *even imperfect* — that's the difference between MVP and unusable. He doesn't revert to the engine-capture workaround because the imperfect view is still enough.

**Capabilities revealed.**

- Graceful degradation on non-standard rigs: no host crash, no DLL unload, the viewer renders whatever the parser can give it.
- **Reset Camera** action in the panel toolbar — recover from arbitrary mesh orientations/scales.
- Implicit growth-feature signal: per-project axis remap is a known future need (logged for backlog, not MVP).

### Journey 3 — Maya finds it on ReaPack (secondary user, community download)

**Persona.** Maya, mid-level sound designer at a 30-person Spanish game studio, 4 years experience, Reaper-native, foley-focused. She's not in Demute's network — she discovered the tool via a "Best Free Reaper Tools for Game Audio" curated list on A Sound Effect. Her studio uses Unity. She opens the Unity editor five times a day to capture animation references.

**Opening scene.** She skims the README on the FBXAnimationViewer GitHub repo. Two lines hook her: *"Score 3D animation live in Reaper — load the glTF, scrub the transport, and skip the engine-capture roundtrip entirely."* and the tagline *"See what changed, hear what should change — directly in Reaper."* That's exactly her pain.

**Rising action.** She installs via the ReaPack repository: subscribes to Demute's repo URL, clicks "Install FBXAnimationViewer". ReaPack notifies her that `cfillion/reaimgui` is a dependency and will be installed alongside; she accepts. Both extensions install in one click. Restart Reaper. The Action `FBXAV: Open Viewer Window` appears in her action list. She finds an old `.glb` from a side project, drops it on a track. An item appears. She hits play. The ReaImGui viewer panel opens, undocked initially — she drags its tab into Reaper's right docker. The mesh animates. She rotates the camera. *This is exactly what she wanted.*

**Climax.** She tries it on a real export from her current Unity project. Her engine exports FBX, not glTF, and one of her characters has an unusual rig her engine accepts but assimp parses incorrectly — bones spin in place, mesh stays mostly intact but a couple of limbs look detached. The viewer didn't crash, but the animation is unusable for this fixture. She opens the project README again: *"Distributed as-is, no SLA. FBX edge cases via assimp's parser may produce incorrect skinning — file an issue with the offending FBX attached if you want to contribute a fixture."*

**Resolution.** She files a GitHub issue with the offending FBX. She knows the no-SLA contract — she filed because she's a contributor type, not because she expects a fix. Demute might address it within three months, might not. Meanwhile, she uses the tool on the 4 of 5 fixtures it handles correctly, saving twenty minutes per session. Net positive. She mentions the tool on her studio's Slack.

**Capabilities revealed (and contracted).**

- ReaPack distribution: one-click install via Demute's repo, ReaImGui auto-installed as declared dependency.
- README sets expectations honestly: as-is, no-SLA, contribution welcome.
- Panel is dockable into any Reaper docker (right, bottom, left, top, or floating) per user preference.
- GitHub issue tracker exists for fixture reports — closes the loop from community fixture corpus back into Demute's regression suite if the team chooses to act on them.
- Failure mode is *non-lethal*: bad fixture renders glitched but the host stays up, allowing the user to switch fixture and continue working.

### Journey Requirements Summary

The three journeys map to these capability areas in priority order:

| Capability | Journey | Phase |
|---|---|---|
| ReaImGui dockable panel (replaces standalone window) | J1, J2, J3 | Phase 0.5 |
| PCM_source plugin: drop `.glb`/`.gltf`/`.fbx` on track → item with correct length | J1 | Phase 3 |
| Item-relative playhead → animation time mapping | J1 | Phase 3 |
| Diffuse-textured Blinn-Phong rendering (material distinction) | J1 | Phase 1 |
| Skinned animation playback | J1 | Phase 2 |
| Camera orbit/zoom/pan + **Reset Camera** toolbar button | J1, J2 | Phase 1, 4 |
| Project state serialization (camera + item bindings + dock position) | J1 | Phase 4 |
| Graceful degradation on malformed/non-standard rigs (no host crash) | J2, J3 | All phases, NFR |
| ReaPack distribution with ReaImGui auto-install dependency | J3 | Phase 5 |
| GitHub issues / fixture corpus collection (post-MVP optional) | J3 | Post-MVP |

The "growth features" backlog (post-MVP, captured separately in Product Scope above) is fed by the implicit needs in J2 (per-project axis remap) and J3 (community fixture coverage).

## Domain Notes — Animation Pipeline Conventions

Demute consumes animations from multiple game-engine pipelines (Unity, Unreal, Godot, proprietary). These pipelines disagree on conventions that the viewer must absorb gracefully without per-project user configuration in MVP.

| Convention dimension | Common variants encountered | MVP behavior | Growth path |
|---|---|---|---|
| Up-axis | Y-up (glTF canonical, Unity), Z-up (Unreal, Blender, some FBX exports) | Render as-authored; user uses camera to compensate | Per-project axis remap (post-MVP) |
| Unit scale | Meters (glTF canonical, Unreal), centimeters (some Maya/3ds Max FBX), millimeters (rare) | Render as-authored; camera Reset frames the rig at any scale | Per-project scale hint (post-MVP) |
| Bone names | English (`Hips`, `LeftShoulder`), French (`Bassin`, `epaule_gauche`), language-mixed, prefixed by `mixamorig:`, etc. | Ignored — viewer doesn't introspect bone semantics, only the skinning matrix palette | N/A — bone-aware features (retargeting, IK display) are out of scope per Non-Goals |
| Texture packaging | GLB embedded (preferred), glTF + sibling files, FBX embedded `aiTexture*`, FBX with external paths | All three loader paths implemented in Phase 1 | N/A |
| Animation channels present | Translation + rotation only, full TRS, root-motion baked or not | All TRS channels sampled; root motion plays as authored | N/A |

**Implicit contract with contributors and community downloaders:** the viewer is *convention-tolerant by camera and graceful degradation, not by automatic remapping*. A rig that renders sideways or scaled wrong is a successful render — the user can still verify timing and identity. Automatic axis/scale remap is a known growth feature, not a Phase 0–5 commitment.

## Desktop Extension Specific Requirements

### Project-Type Overview

FBXAnimationViewer ships as a native Reaper extension DLL — not a standalone application, not a process, not a service. Its lifecycle is bound to Reaper's process: loaded at host startup, unloaded at host shutdown, never running in isolation. This shapes every technical decision below.

### Platform Support

| Platform | MVP commitment | Phase | Notes |
|---|---|---|---|
| Windows x64 | ✅ Yes | Phases 0–5 | MSVC 19.44+ via Visual Studio 2022; WGL context (Win32 native + sokol_gfx GL backend). Validated 2026-05-09 in Phase 0. |
| macOS (Intel + Apple Silicon) | ❌ No (post-MVP) | Growth | ReaImGui's internal SWELL handles UI portability; sokol_gfx has a Metal backend. Rough estimate 25–30% additional effort per PRFAQ. |
| Linux x64 | ❌ No (post-MVP) | Growth | Same architectural basis as macOS (SWELL via ReaImGui + sokol_gfx GL). |

CMake configures cleanly on non-Windows in Phase 0 (stub target). Only Windows is required to actually link in Phases 0–5.

### System Integration

| Subsystem | Integration |
|---|---|
| Reaper host process | DLL loaded via `%APPDATA%\REAPER\UserPlugins\`. Entry: `REAPER_PLUGIN_ENTRYPOINT`. API resolution via `rec->GetFunc("FunctionName")`. |
| ReaImGui extension (runtime dependency) | Function-pointer resolution via `rec->GetFunc("ImGui_*")`. If ReaImGui is missing at load, the extension emits a console message instructing the user to install `cfillion/reaimgui` and bails cleanly without registering its action. |
| ReaPack distribution channel | Manifest at `reapack/index.xml`. Auto-install dependency declared on `cfillion/reaimgui`. One package per platform/arch from Phase 5 onward. |
| File system | Read-only access to glTF / GLB / FBX files via assimp's `Importer::ReadFile`. No writes outside Reaper's project ext-state (Reaper-owned). |
| Operating system | No global hotkeys outside Reaper actions. No clipboard. No shell exec. No background process. No taskbar tray. |

### Update Strategy

Updates are **100% delegated to ReaPack**. The extension itself has zero update logic: no version check at startup, no nag dialog, no "download now" button. When Demute pushes a new version to the ReaPack manifest, users receive a notification through their normal ReaPack workflow and one-click upgrade.

Implication: every released version must be self-contained and backwards-compatible with Reaper project files saved by prior versions. Forward-compatibility is opportunistic — a newer extension reading an older project's ext-state should succeed; an older extension reading a newer project's ext-state may ignore unknown fields without erroring.

### Offline Capabilities

The extension is **100% offline**. Zero network traffic of any kind:

- No telemetry, analytics, error reporting.
- No license check, no online activation.
- No remote asset loading; all glTF/FBX/GLB content is read from local disk.
- ReaPack itself handles network for install/update but runs as a separate workflow; our DLL never opens a socket.

The viewer must be fully functional on an air-gapped workstation provided the user has copied the DLL, installed Reaper, and installed ReaImGui locally.

### Implementation Considerations

- **C++17, MSVC ABI on Windows.** No mingw, no clang-cl on the Windows build — matches the Reaper SDK's documented requirement for the MSVC-compatible C++ ABI on Win32.
- **No exceptions in plugin entry path** (`REAPER_PLUGIN_ENTRYPOINT`). Reaper is a hostile host: an exception escaping our entry point can crash the entire DAW. MSVC's default `/EHsc` is left on for now since no exception sites exist in our code; revisit if assimp or sokol_gfx expose exception-throwing APIs.
- **Single-threaded UI, single-threaded GL.** ReaImGui callbacks run on Reaper's main thread; sokol_gfx's GL context is bound on the same thread. Worker threads (file loading, hashing) are out of scope for MVP — added later if a Demute fixture's load time exceeds 1 second.
- **Memory model.** The extension owns its allocations. Reaper retains pointers we pass to `Register` (gaccel, hookcommand, PCM_source factory) — those are static-lifetime within our DLL. On unload (`rec == nullptr`) we deregister with the `-` prefix convention to avoid Reaper dereferencing freed function addresses (symmetric unregister already applied in Phase 0).
- **Project state persistence.** Per-item config (animation file path, camera state, time offset/scale) round-trips through the PCM_source's `SaveState`/`LoadState` methods, which Reaper invokes during project save/load. Panel dock position is handled by ReaImGui's own state persistence.

## Project Scoping & Phased Development

### MVP Strategy & Philosophy

**MVP type: problem-solving.** FBXAnimationViewer targets one specific, daily, quantified workflow pain — the engine-capture roundtrip. Success is binary per session: Antho replaces the workaround or he doesn't. No experience MVP (we're not optimizing first-impression for casual users), no platform MVP (we're not building infrastructure for unknown future features), no revenue MVP (it's free).

This shapes scope discipline. Every feature must directly serve *"Antho ships the engine-capture-replacement workflow"*, not *"could be cool for someone someday"*. The cuts negotiated during party mode (Phase 4 file-watcher → manual reload button, FBX edge cases → tolerated 60% compatibility, normal maps → growth, Camera reset → bare minimum) all follow this single lens.

**Resource model: solo, validator-bound.** Implementing developer: Claude Opus 4.7. Validator: Antho. No additional headcount, no external contracting, no design/QA hand-offs. Critical path = validator turnaround time per phase gate, not coding throughput. This is locked since PRFAQ; revisit only if Antho's validator availability changes during the 6-week window.

### MVP Feature Set & Phased Roadmap

The MVP feature set and the post-MVP roadmap are enumerated in the **Product Scope** section above (MVP / Growth Features / Vision). Phase-by-phase delivery is enumerated in the PRFAQ "6-Week Build Plan" plus the Phase 0.5 insertion (ReaImGui refactor) from the architectural decision in step-04.

Single source of truth: the **Product Scope** section above defines *what* ships. This Scoping section captures *why* and *under what risk profile*.

### Risk Mitigation Strategy

**Technical risks (consolidated from PRFAQ + party-mode Winston):**

| Phase | Risk | Mitigation |
|---|---|---|
| 0 | Reaper SDK first-run friction (Cockos docs sparse) | ✅ Mitigated — Phase 0 shipped 2026-05-09, 7 of 8 validator-gate rows green. |
| 0.5 | ReaImGui ↔ sokol_gfx FBO interop on main thread | Validate render-to-texture + `ImGui::Image()` path on day 1 of Phase 0.5; if blocking, fall back to ImGui DrawList custom callback (last resort). |
| 1 | assimp + sokol_gfx integration (texture loading paths, coordinate conventions) | Start with Khronos glTF sample model; validate winding/UV/tangent conventions early. Anchor in glTF-canonical conventions; document divergences per Domain Notes. |
| 2 | **Phase 2 is the dominant technical risk** — skinned animation correctness (bone hierarchy + matrix palette + GPU vertex skinning). 7–10 days, not 5 (Winston party-mode estimate). | Validate against Blender / FBX Review per fixture. Test with Demute's real FBX exports, not Khronos samples only. |
| 2 | Coordinate system assimp → GPU (row vs column-major, bind pose inverse, pre/post-rotation FBX) | Pin to one convention (column-major for GLSL canonical); explicit conversion at the assimp boundary; document the transform pipeline. |
| 3 | Reaper transport API subtleties (project tempo, time signature, timecode flavor) | Stick to seconds-from-project-start; ignore tempo/sig until Phase 5 if needed. |
| 3 | PCM_source plugin implementation depth — virtual methods, ext-state serialization | Reference: existing Reaper extensions using PCM_source (look at ReaImGui's own item-related patterns where applicable). |
| 4 | Project-level state persistence API may be limited in Reaper SDK | Fallback: store camera/binding state in per-track ext-state or project notes. |
| 5 | FBX edge cases via assimp (some FBX exports break assimp's parser) | Test with Demute's actual FBX exports during Phase 2, not at the end. 60% compatibility accepted in MVP. |

**Market risk:** competitors emerging post-Phase-0. Mary's party-mode candidate (Wwise/Unreal animation preview) was retired by Antho clarifying *middleware is out of scope*. The residual risk is a ReaImGui community script implementing glTF rendering — Mary flagged investigation as *obligatoire* before Phase 1 starts (search the Cockos forum + ReaPack index for `glTF`, `fbx`, `animation viewer` keywords; reuse if a clean brick exists).

**Resource risk:** validator (Antho) availability is the critical path. If Antho's bandwidth contracts during Phases 2–3 (the long phases), the project slips proportionally — there is no buffer to absorb more than ~2 days/week of validator unavailability. Mitigation: honest re-estimation at each phase gate, not optimism.

### Hypothetical Scope-Cut Hierarchy (if dérapage)

These are *contingencies*, not committed changes. If the 6-week window comes under pressure, cuts apply in this order:

1. **FBX Phase 4** → defer to v1.1 post-release (glTF/GLB-only at MVP release).
2. **Auto file-watcher (Phase 4)** → already downgraded to "P2 polish"; first to drop entirely if Phase 4 needs to slim further.
3. **Camera state persistence across project save/load** → fall back to per-Reaper-session only (state resets when Reaper closes).
4. **PCM_source `SaveState`/`LoadState` (Phase 3)** → fall back to manual per-item rebinding on project reopen.

Anything below this line is the irreducible MVP per problem-solving philosophy — it's the engine-capture replacement loop, and cutting it kills the project.

## Functional Requirements

### Animation Loading & Format Support

- **FR1**: The sound designer can load animation files in glTF 2.0 binary container (`.glb`) format.
- **FR2**: The sound designer can load animation files in glTF 2.0 multi-file (`.gltf` + `.bin` + textures) format.
- **FR3**: The sound designer can load animation files in FBX format, with at least 60% of common Demute exports rendering correctly.
- **FR4**: The sound designer can load animation files whose internal coordinate convention differs from the glTF-canonical (Y-up vs Z-up, meters vs centimeters) without per-file pre-configuration.
- **FR5**: The sound designer can load animation files using any bone-naming convention, including non-English names.
- **FR6**: The viewer can render a static mesh when the loaded file contains no animation channels.
- **FR7**: The viewer can sample animation channels for translation, rotation, and scale per bone, including root motion when present in the source file.

### Timeline Integration

- **FR8**: The sound designer can drag an animation file (`.glb`, `.gltf`, `.fbx`) from any source onto a Reaper track to create a media item bound to that animation.
- **FR9**: The viewer can create a Reaper media item whose timeline length matches the animation's duration.
- **FR10**: The viewer can map the Reaper playhead position to the animation's playback time using item-relative offset (`animTime = playheadTime − itemStart`, clamped to `[0, itemLength]`).
- **FR11**: The sound designer can position, move, resize, color, and rename animation items using Reaper's native item controls, identically to other Reaper media items.
- **FR12**: The viewer can coexist with other media types (audio, video, MIDI) on the same Reaper session without interfering with their playback.
- **FR13**: The viewer can support multiple animation items across one or more tracks in the same Reaper project.
- **FR14**: The viewer can determine which animation to display based on the animation item currently spanning the playhead position; if multiple items overlap, it displays the one on the highest-priority track.

### 3D Rendering & Visual Fidelity

- **FR15**: The viewer can render skinned mesh geometry with per-frame bone deformation.
- **FR16**: The viewer can render mesh surfaces using diffuse-texture-mapped shading with per-material specular response sufficient to distinguish material types (e.g., matte leather vs polished steel).
- **FR17**: The viewer can resolve textures embedded in a GLB binary container.
- **FR18**: The viewer can resolve textures referenced as external sibling files in multi-file glTF.
- **FR19**: The viewer can resolve textures embedded in FBX containers.
- **FR20**: The viewer can render meshes composed of multiple materials, each rendered with its own material parameters.
- **FR21**: *(recategorized as NFR-P1 — 60 fps is a quality attribute, not a capability.)*

### Camera & Viewport Control

- **FR22**: The sound designer can orbit the camera around the rig with right-click drag inside the viewer panel.
- **FR23**: The sound designer can zoom the camera with mouse scroll inside the viewer panel.
- **FR24**: The sound designer can pan the camera with middle-click drag inside the viewer panel.
- **FR25**: The sound designer can reset the camera to a default framing that contains the rig's bounding box, via a dedicated toolbar button in the panel.
- **FR26**: The viewer can continue updating the displayed animation frame while the sound designer manipulates the camera (no pause-on-interaction).

### Panel & Docking

- **FR27**: The viewer presents its preview inside a ReaImGui-driven panel.
- **FR28**: The sound designer can dock the viewer panel into any Reaper docker (top, bottom, left, right, floating) using ReaImGui's native docking gestures.
- **FR29**: The viewer can register a Reaper Action (`FBXAV: Open Viewer Window`) that opens the panel when triggered.
- **FR30**: The viewer can detect, on extension load, whether ReaImGui is installed, and emit a user-readable console diagnostic if it is missing — failing gracefully without registering its Action.

### Project State Persistence

- **FR31**: The viewer can serialize per-item state — animation file path, camera angle, optional time offset and scale — into the Reaper project file so it survives project save/load.
- **FR32**: The viewer can serialize the viewer panel's dock position into the Reaper project file (delegated to ReaImGui's own state persistence).
- **FR33**: The sound designer can reopen a previously saved Reaper project and find each animation item's binding and viewport state restored without manual reconfiguration.

### Error Tolerance & Graceful Degradation

- **FR34**: The viewer can avoid crashing the Reaper host when fed a malformed, unsupported, or partially parseable animation file, instead displaying the file as best the parser allows.
- **FR35**: The viewer can emit a user-readable console diagnostic when an animation file fails to load, including the file path and an error category.
- **FR36**: The sound designer can manually reload the animation file bound to a given item via a "Reload" button in the panel, picking up changes on disk since the last load.
- **FR37**: The viewer can continue operating after a single animation item fails to load, without affecting other items in the same session.

### Distribution & Operation

- **FR38**: The sound designer can install the viewer via ReaPack from the Demute repository as a one-click action that also installs `cfillion/reaimgui` as an auto-dependency.
- **FR39**: The sound designer can install the viewer manually by copying its DLL to `%APPDATA%\REAPER\UserPlugins\` (provided ReaImGui is already installed) and have it fully functional after the next Reaper restart.
- **FR40**: The viewer can operate fully offline: no network access, no telemetry, no remote license check, no remote asset loading.
- **FR41**: The sound designer can receive updates to the viewer via ReaPack's standard update flow; the viewer has no in-extension update mechanism of its own.

## Non-Functional Requirements

### Performance

- **NFR-P1**: The viewer renders at ≥60 frames per second on the validator's reference Windows workstation for fixtures up to ~20k triangles, 4 materials, 50 bones. (Recategorized from FR21.)
- **NFR-P2**: Initial load of a typical Demute glTF/GLB fixture (≤20k tris, ≤4 materials, ≤50 bones, embedded textures totaling ≤8 MB) completes within 2 seconds from the file drop to the first rendered frame.
- **NFR-P3**: Manual reload of an already-loaded animation, when the source file has been replaced on disk, completes within 1 second from the Reload button click to the first updated rendered frame.
- **NFR-P4**: The DLL loads at Reaper startup in under 2 seconds (Phase 0 baseline maintained throughout MVP).
- **NFR-P5**: Transport scrubbing latency — the displayed animation frame matches the Reaper playhead position within one rendered frame (≈16 ms at 60 fps) under normal load.
- **NFR-P6 (capacity)**: The extension supports at least 10 simultaneous animation items in a Reaper project without measurable performance degradation on the validator's reference workstation. Fixtures up to ~50k triangles render correctly but may exhibit lower frame rates.

### Reliability

- **NFR-R1**: Zero Reaper-host crashes attributable to FBXAnimationViewer during Demute internal usage from Phase 4 onward. (Aligned with Success Criteria > Technical Success.)
- **NFR-R2**: No Reaper-project data loss caused by the extension. The extension must not corrupt the project file, drop tracks, or invalidate item references during save/load.
- **NFR-R3**: Extension unload (`rec == nullptr`) is clean: no leaked GL contexts, no leaked window classes, no dangling Reaper API pointers, no orphaned timers. Symmetric registration/deregistration verified for every call to `rec->Register` made at load.
- **NFR-R4**: Failure to load an animation file is non-lethal: the host stays up, other animation items in the same session keep working, and the user receives a diagnostic on the Reaper console.
- **NFR-R5**: The build remains warning-free at MSVC `/W3 /permissive-`. Any new warning surfaced by a phase must be resolved before the phase gate is approved.

### Compatibility & Integration

- **NFR-C1**: The extension targets Reaper 7.x with `caller_version == REAPER_PLUGIN_VERSION` of the SDK at build time (currently `0x20E`, SDK update 2026-05-07 for Reaper 7.72). Loading on older Reaper builds fails cleanly with `return 0` from the entry point.
- **NFR-C2**: The extension declares ReaImGui (`cfillion/reaimgui`) as an auto-install ReaPack dependency. On a fresh user environment, installing FBXAnimationViewer pulls ReaImGui automatically.
- **NFR-C3**: The Windows build targets the MSVC x64 ABI exclusively. No mingw, no clang-cl, no 32-bit. Required by the Reaper SDK's C++ ABI compatibility note (`reaper_plugin_functions.h:27`).
- **NFR-C4**: glTF 2.0 conformance follows assimp's parser capabilities for the pinned commit recorded in `extern/VENDORED.md`. Re-vendoring assimp requires re-validation against the Demute fixture corpus.
- **NFR-C5**: FBX support follows assimp's parser capabilities for the pinned assimp version. The 60% compatibility target in FR3 applies to a representative sample of recent Demute client FBX exports, not to all FBX files in the wild.

### Out of Scope (NFR Categories Deliberately Skipped)

- **Security**: no sensitive data, no network surface, no auth flow, no payment processing, no IP protection beyond the MIT-licensed source itself. The single security-adjacent concern (host crash → data loss) is captured under Reliability NFR-R1 and NFR-R2.
- **Scalability**: single-user, single-machine. The only "scale" axis is items per project, captured under Performance NFR-P6.
- **Accessibility**: deferred to post-MVP. The viewer inherits ReaImGui's default accessibility behaviors (keyboard navigation, font scaling) without explicit additional work. Revisit at Phase 5 if Demute receives accessibility-related feedback from the community.
