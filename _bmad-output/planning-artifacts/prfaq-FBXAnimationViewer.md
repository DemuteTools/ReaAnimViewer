---
title: "PRFAQ: FBXAnimationViewer"
status: complete
created: "2026-05-06"
updated: "2026-05-09"
stage: "verdict"
inputs:
  - source: "user-provided concept brief"
    summary: "Cross-engine animation viewer for sound designers — Reaper extension loading FBX/glTF/Alembic and rendering animated mesh synced to Reaper playhead. Primary user: Demute studio. Free release to game audio community. MVP estimate 4-6 weeks."
  - source: "web-researcher subagent"
    summary: "Whitespace VALID. Cockos forum thread 192268 (2017) is open demand evidence — 8 years unanswered. No Reaper extension/script in ReaPack, SWS, GitHub awesome-reaper, KVR, A Sound Effect's curated lists, or BØLT loads animated rigs synced to transport. Adjacent solutions (Autodesk FBX Review, F3D, MotionBuilder, Wwise 3D Viewer) exist but lack DAW transport sync or render the wrong thing. Demute already publishes 9 Reaper tools + 7 Unreal tools via Toolbox and is cited in A Sound Effect's 'Best Free REAPER Tools for Game Audio'. Game sound design market $1.2B (2024) → $2.5B (2033), ~9.1% CAGR. Reaper described as 'industry standard in game audio'. Risk: FBX SDK redistribution constraints for open-source — glTF primary recommended. Risk: native C++ Reaper extension + embedded GL/Vulkan/D3D context is non-trivial cross-platform work."
---

<!-- coaching-notes-stage-1 -->
**Concept type:** Hybrid — internal Demute tool released free to the game audio community (Demute "open tools" model). Calibrate FAQ for community/free-release framing, not commercial unit economics. Stakeholder value = Demute's own production efficiency + community goodwill + recruiting/reputation halo.

**Customer-first reframe:** User initially led with form factor ("VST plugin"). Redirected to customer pain. The deepest pain isn't time loss from manual capture — it's the **camera angle being locked at capture time**, forcing recapture or designing blind to angles that matter. Quote from user: *"bouger la caméra dans Reaper serait INCROYABLE gain de qualité"*. This is a feature unlock, not just an efficiency gain. The press release must lead with this.

**Quantified pain:** Up to 20 video captures per character. Re-done every animator iteration. Re-recorded if PC stuttered during capture. Multiple times per week.

**Form factor decision:** Started as "VST" (suggested blindly by another agent), corrected to **native Reaper extension distributed via ReaPack**. Reason: cross-engine = file format agnostic (NOT cross-DAW), game audio market is ~Reaper-dominant, ReaPack is the idiomatic distribution channel, and Reaper extension SDK supports the embedded 3D rendering context required. Multi-DAW reach was a faux benefit for this use case.

**Cross-engine semantics:** Means cross-game-engine via interchange files (FBX/glTF/Alembic exported from Unity/Unreal/Godot/proprietary). NOT multi-DAW. Workflow assumes animations are exported to interchange format — engine-native animations require a re-export step.

**MVP scope decision:** glTF + FBX for MVP. Alembic in phase 2 (niche for game audio, more VFX/cinematics). Web researcher recommends **glTF as primary** to avoid Autodesk FBX SDK redistribution constraints on open-source release; FBX support possibly via FBX2glTF preprocessing or assimp permissive parser — implementation choice deferred to architecture stage.

**Why now (timing):** Reaper 7.37 (May 2025) added new render-window and preview-playback features. glTF 2.0 became ISO/IEC 12113:2022. Wwise 2025.1 added 3D viewers (not for animation but normalises the "3D in audio tool" UX pattern). 8-year forum-thread silence with no solution shipped indicates the rare skill combination (game-audio insider + Reaper extension dev + 3D engine literacy) — Demute fits.

**Subagent findings unused / deferred:**
- Independent quantified demand (Reddit/Slack download metrics) — gap. Worth a poll in gameaudio.slack post-MVP.
- Demute Toolbox download/install telemetry — request from Antho if available for marketing.
- Format risk on FBX SDK redistribution — flag for architecture review.

---

# Demute Releases Asset Animation Viewer: 3D Character Animation, Live Inside Reaper, From Any Angle

## Game audio sound designers can now load, scrub, and freely navigate animations exported from any engine — without recording a single screen capture.

**Mons, Belgium — [Launch Date 2026]** — Demute today released **Asset Animation Viewer**, a free Reaper extension that lets game audio sound designers see and scrub through 3D character animations directly inside their DAW. The extension loads source animation files — glTF and FBX exported from Unity, Unreal, Godot, or any proprietary engine — and renders the animated rig in lock-step with the Reaper playhead. For the first time, sound designers can scrub the timeline AND move the camera freely while designing, exposing details no pre-recorded video would show.

Sound designing for game characters today means juggling two tools and a workflow that breaks every time an animator iterates. To time a footstep, a sword swing, or a magic cast, designers leave Reaper, open the engine, queue the animation, screen-record it, and import the resulting MP4 onto a Reaper video track. A single character can require twenty separate captures — one per animation. If a frame stutters during the recording, the take is unusable. When the animator tweaks a kick by three frames the next morning, every dependent capture has to be recaptured from scratch. And once a take is on the timeline, the camera angle is frozen — if the gesture you need to time is occluded behind the character's body, the only fix is going back to the engine.

Asset Animation Viewer collapses that loop into a single Reaper extension. Drop a glTF or FBX file onto a track and the animation appears, attached to the playhead. Scrub the timeline and the rig follows — frame-accurate, with no video file involved. When an animator delivers a new version, the source file replaces the old one and every downstream sound stays in sync. And because the rig is rendered live, designers rotate, pan, and zoom the camera while they work — surfacing micro-gestures the original capture would have hidden.

> "For eight years, our team of around ten has been recording videos of animations we already had source files for. The step itself was tedious; the worse cost was angles. Every recording freezes one perspective on something three-dimensional, and small retakes — frequent on some projects — meant re-recording the lot. Asset Animation Viewer removes the step. We design to the animation itself now, not a flattened recording of it. We're releasing it free because the game audio community taught us most of what we know."
> — Antho [Lastname], [Title], Demute Studio

### How It Works

1. Install Asset Animation Viewer through ReaPack by adding the Demute repository.
2. Add a track to your Reaper project and drop a `.gltf` or `.fbx` file onto it — the same kind of file your animation team exports from their engine of choice.
3. The viewer panel opens, attached to the track. The character's skeleton and mesh appear in their first-frame pose.
4. Press play. The animation runs in lock-step with Reaper's transport — start, stop, scrub, loop, all frame-accurate.
5. Right-click and drag to orbit the camera. Scroll to zoom. Save the camera state per project so you return to the right angle on every session.
6. When the animator delivers a new export, replace the source file. The viewer reloads; downstream sounds stay timed.

### Getting Started

Asset Animation Viewer is free and open-source. Install it through ReaPack by adding the Demute repository, or grab the latest release at `github.com/demute/asset-animation-viewer`. Documentation, supported format details, and contribution guidelines live there. Released under [License TBD] for the game audio community.

---

## MVP Scope — Decisions

| # | Item | MVP Status | Notes |
|---|---|---|---|
| 1 | Engine-runtime-composed animations (state machines, blend trees, runtime IK, additive layers) | Out — bake required by animator | Documented prerequisite. Not Demute's burden. |
| 2 | VFX, particles, shaders, lighting | Out — accepted limitation | Tool is a "rig viewer". Skeleton + skinned mesh only. |
| 3 | Multi-rig simultaneous (one file per track) | In | Track-bound architecture. |
| 4 | Engine cinematic compositions (Sequencer cuts, etc.) | Out | User aligns baked rigs manually on Reaper tracks. |
| 5 | Retargeted skeletons, root motion (when present in source file) | In | If it's in the file, it plays. |
| 6 | Camera orbit / zoom / state persistence per project | In — core feature unlock | The reason this tool exists, per user testimony. |
| 7 | Per-track viewer toggle, separate window with isolated GL context | In | Decouples from audio DSP load. |
| 8 | File-watcher auto-reload (animator delivers new version → sync preserved) | In | One of the two killer features. |
| 9 | glTF format support | In | Open standard. |
| 10 | FBX format support | In via assimp | Permissive open-source parser, avoids Autodesk SDK redistribution constraints. |
| 11 | Alembic format support | Phase 2 | Niche for game audio. |
| 12 | Platforms | Windows MVP, architected portable | Cross-platform port = 25-30% additional effort, not a rewrite. |

## Build Stack

- **Language:** C++17
- **Reaper integration:** Reaper Extension SDK + SWELL
- **3D rendering:** sokol_gfx (header-only, cross-platform, minimal boilerplate)
- **Animation parsing:** assimp (FBX + glTF in one library)
- **Math:** GLM
- **Build system:** CMake
- **Window/GL context:** SWELL window + native GL context (Win32 wgl on Windows MVP)
- **Distribution:** ReaPack repository (Demute)

**Developer:** Claude Opus 4.7 (autonomous coding agent). **Validator:** Antho (Reaper integration testing, animation files for fixtures, UX feedback). **Iteration cadence is bound by validator turnaround time.**

## 6-Week Build Plan

### Phase 0 — Scaffolding (Week 1)
- Repo setup, CMake build system, Reaper Extension SDK boilerplate
- "Hello Reaper" extension that registers and loads in Reaper
- SWELL window with empty GL context that opens on extension load
- ReaPack manifest scaffolding
- **Validator gate:** Antho confirms the extension loads in Reaper without crashing and the window opens.

### Phase 1 — Static Mesh Rendering (Week 2)
- Integrate assimp + sokol_gfx
- Load a glTF file from disk via UI button
- Render static mesh (no animation, no skinning yet) with default lighting
- Implement camera: orbit (right-click drag), zoom (scroll), pan (middle-click drag)
- **Validator gate:** Antho drops a glTF, sees the static mesh, can rotate camera around it.

### Phase 2 — Skinned Animation Playback (Week 3)
- Implement skinned mesh rendering: bone hierarchy traversal, matrix palette, GPU vertex skinning
- Sample animation channels (translation, rotation, scale per bone)
- Manual play/pause/scrub via debug UI
- Test fixture: provided glTF rigged character with at least one animation
- **Validator gate:** Antho drops an animated glTF, plays the animation manually, validates correctness against the original engine source.

### Phase 3 — Reaper Transport Sync (Week 4)
- Hook Reaper transport API (`GetPlayPosition`, play/pause/stop callbacks)
- Map Reaper playhead time → animation time with configurable offset and time scale per track
- Track-binding: drop a glTF/FBX onto a Reaper track to attach it to that track
- Per-track enable/disable toggle
- **Validator gate:** Antho scrubs Reaper timeline, animation follows frame-accurate. Multi-track test with two simultaneous rigs.

### Phase 4 — File Reload + Camera Persistence (Week 5)
- File-watcher per loaded source: detect external modifications, auto-reload
- Preserve current frame and camera state across reloads
- Save/restore camera state per Reaper project (using project-level state hooks)
- FBX support enabled via assimp (glTF was Phase 1; this turns FBX on, tests edge cases)
- **Validator gate:** Antho saves a project, closes Reaper, reopens — camera restored. Modifies a glTF on disk, viewer reloads automatically.

### Phase 5 — Polish + Release (Week 6)
- UI polish: track-binding affordances, camera reset, format error messages
- Performance pass: profile multi-rig scenes, optimize hot paths
- ReaPack packaging, README, install instructions
- Release notes, GitHub repo public, ReaPack manifest published
- **Validator gate:** Real Demute project test — load a real character, design real sound to it, validate no regressions vs. screen-capture workflow.

## Risks (per phase)

| Phase | Risk | Mitigation |
|---|---|---|
| 0 | Reaper Extension SDK first-run friction (Cockos docs sparse) | Lean on community examples (SWS source, ReaImGui source). Buffer: 1-2 days. |
| 1 | assimp + sokol_gfx integration weirdness (texture loading, coordinate conventions) | Start with Khronos glTF sample model. Validate winding/UV/tangent conventions early. |
| 2 | Skinned animation correctness (bone hierarchy + matrix palette + GPU skinning) | Most error-prone phase. Validate against known-good viewer (Blender, FBX Review) per fixture. |
| 3 | Reaper transport API subtleties (project tempo changes, time signature, timecode flavor) | Stick to seconds-from-project-start; ignore tempo/sig until phase 5 if needed. |
| 4 | Project-level state persistence API may be limited in Reaper SDK | Fallback: store camera state in project notes or per-track ext state. |
| 5 | FBX edge cases via assimp (some FBX exports break assimp's parser) | Test with Demute's actual FBX exports during Phase 2, not at the end. |

## Open Items / Deferred Decisions

- **Naming:** "Asset Animation Viewer" working name — defer final naming to launch.
- **License:** MIT or Apache-2.0 (Demute toolbox default — confirm).
- **GitHub URL:** TBD.
- **Mac/Linux ports:** Post-MVP, community demand-driven.
- **Alembic support:** Phase 2 if requested.
- **VFX/particle support:** Out of scope; possibly future module if community demand.
- **Engine-side bake helpers (Unreal/Unity scripts to export runtime anim → FBX):** Future work, not in MVP.

---

## Concept Verdict

**Forged in steel:**
- Customer pain is real and quantified — up to 20 captures per character, frequent retake cycles, locked camera angles. The deepest value is the **camera unlock**, not the time savings.
- Whitespace verified: 8-year-old Cockos forum thread (#192268) is direct demand evidence; no existing Reaper extension or script renders animated rigs synced to transport.
- Form factor (Reaper extension via ReaPack) corrected from initial VST suggestion; aligns with game-audio-Reaper-dominated market and Demute's existing distribution channel (Toolbox + ReaPack, 9 active Reaper tools).
- MVP scope is tight and defensible: rig viewer only, glTF + FBX via assimp, Windows, single-track binding.
- Demute is uniquely positioned: 8-year studio + community distribution track record + scratch-your-own-itch use case = structural maintenance commitment.

**Needs more heat:**
- Phase 2 (skinned animation correctness) is the dominant technical risk. Bone hierarchy + matrix palette + GPU vertex skinning is non-trivial; validation against Blender / FBX Review per fixture is required.
- Reaper Extension SDK + SWELL + native GL context combination has limited public reference — first-run friction expected (1-2 day buffer in Phase 0).
- Validator turnaround time (Antho's availability for testing each phase gate) is the schedule's true critical path, not coding throughput.

**Cracks watched:**
- FBX support quality depends on assimp's behaviour with arbitrary FBX exports — Demute's real fixtures must be tested in Phase 2, not deferred to Phase 5.
- "Engine cinematic compositions out of scope" must be communicated honestly — sound designers timing magic VFX or particle bursts will still need video reference for those moments.

**Recommendation: GO.** Concept is forged, scope is tight, whitespace is real, distribution channel exists, technical plan is concrete. Start Phase 0 when validator (Antho) is ready to commit weekly gate availability for 6 weeks.

---

<!-- coaching-notes-stage-3 -->
**Customer FAQ exercise was challenged by user as marketing theater for an internal tool.** Reframed as MVP scope decisions. Eight original customer questions surfaced six real scope decisions (engine-runtime anims, VFX, multi-rig, cinematics, retargeting/root motion, platform). Three classic customer FAQ questions (workflow change resistance, long-term maintenance, switching cost) dismissed as N/A for an internal-tool-released-free model.

<!-- coaching-notes-stage-4 -->
**Internal FAQ stage skipped** — feasibility, risks, and trade-offs covered directly in the Build Stack, 6-Week Plan, Risks per phase, and Concept Verdict sections.

<!-- coaching-notes-stage-5 -->
**Build model is unusual:** Claude Opus 4.7 as autonomous developer, user (Antho) as Reaper-integration validator. Critical-path constraint = validator turnaround time per phase gate, not coding throughput.

**Stack choice rationale:** sokol_gfx + assimp + Reaper SDK + SWELL + CMake. Picked sokol_gfx over bgfx for header-only simplicity; over raylib because raylib's window management would conflict with Reaper-owned windows. Picked assimp over a native glTF parser to keep FBX in MVP without Autodesk SDK redistribution risk.

**Deferred / unresolved:** final naming, license, GitHub URL, Mac/Linux platform timing, Alembic support trigger, engine-side bake helpers.
