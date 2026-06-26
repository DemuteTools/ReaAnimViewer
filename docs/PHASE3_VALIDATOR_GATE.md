# Phase 3 — Validator Gate

Step-by-step acceptance test for **Phase 3** (Epic 4 — sync the animation to the
Reaper transport). Written for Antho (the validator) to run on Windows. This file
is the **sole authority on Phase 3 completion** (AR19) and grows as Epic 4
progresses: **Story 4.1** seeds the rows below (the `PCM_source` factory registers
and creates a source from a file — *plumbing only*); Stories 4.2 (correctly-sized
item == real animation duration) and 4.3 (playhead → frame mapping) append their
own.

**You are testing the right thing if**, for Story 4.1, loading ReaAnimViewer prints
a single `[RAV] info: pcmsrc factory registered …` line, **dropping a
`.glb`/`.gltf`/`.fbx`/`.dae` onto a track creates a media item** (a **~1 second
placeholder length is expected and correct** here — see the scope note), dropping a
**non-animation** file (`.txt`, `.wav`) behaves **exactly as Reaper normally would**
(our factory never hijacks it), and **closing the project / unloading / quitting
Reaper does not crash**.

---

## Prerequisites

- Reaper 7.x (`caller_version == 0x20E`, current SDK targets 7.72)
- Visual Studio 2022 with **Desktop development with C++**
- CMake ≥ 3.20 and Git for Windows on PATH
- The same assimp 6.0.5 + GLM 1.0.3 + stb vendored stack as Phases 1–2 — **no new
  dependency** in this story (no new importer, no new API symbol). First configure
  is slow; later builds reuse the cache.

> Phase 3 still has **no ReaImGui dependency** and **no offscreen FBO** — the
> viewport is the same native OpenGL docked window from Epic 1. Story 4.1 is
> **registration + a working source object only**: a new `src/pcm_source_anim.cpp`
> adds the `PCM_source` subclass + `pcmsrc_register_t` factory, and
> `src/plugin_main.cpp` registers/unregisters it symmetrically. **No shader,
> renderer, GL-resource, loader, `scene.h`, `animation.h`, or `reaper_api.h`
> change** — so every Epic 1–3 behavior renders and behaves identically.

## 1. Build & install

Identical to the Phase 1 / Phase 2 gate (§1):

```cmd
cd \path\to\ReaAnimViewer
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
copy build\Release\reaper_animviewer.dll "%APPDATA%\REAPER\UserPlugins\"
```

**Expected:** **zero MSVC warnings at `/W3 /permissive-`** for our own sources, and a
single DLL at `build\Release\reaper_animviewer.dll` with **no `assimp*.dll`** beside
it (assimp is statically linked; stb is header-only). Close and reopen Reaper to
re-scan.

## 2. What you are testing

Story 4.1 makes an animation file **first-class Reaper media**: a `PCM_source`
factory is registered at load, so Reaper knows it can build *our* source from a
`.glb`/`.gltf`/`.fbx`/`.dae` path. **Nothing about playback or sizing is wired yet** —
the source reports a fixed **placeholder length** and produces no audio. The
click-based test is: **load → read the console; drop a file → watch for an item;
unload → confirm no crash.** The console `[RAV]` channel is read in **Reaper's
console** (open it via the action list / ReaScript console if it is not visible).

**Suggested fixtures** (re-use the Phase 1–2 set):
- *Animation files (should claim):* any `.glb`/`.gltf`/`.fbx` rig used in Epic 2–3
  (`Fox`, `CesiumMan`, a Mixamo `.fbx`), plus a `.dae` if on hand.
- *Foreign files (should NOT claim):* any `.wav` / `.txt` / `.mp3` — Reaper must
  handle these exactly as it does without our extension installed.

## 3. Story 4.1 — PCM_source registers & creates a source from a file

Story 4.1 registers the factory and gives Reaper a working source object. **It does
not load the asset, parse duration, touch GL, or sync the playhead** — those are
4.2 / 4.3. The proof it works is simply that **an item appears on drop** (Spike 0
confirmed a 0-channel, silent source still produces a real timeline item).

At load the console shows exactly one new line:

```text
[RAV] info: pcmsrc factory registered (.glb/.gltf/.fbx/.dae)
```

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Factory registered at load | After loading ReaAnimViewer, the console shows **one** `[RAV] info: pcmsrc factory registered (.glb/.gltf/.fbx/.dae)` line. It appears once per load, no duplicate, no `warn`/`error`. | AC1 |
| 2 | Item created from a file | **Drag a `.glb`/`.gltf`/`.fbx`/`.dae` onto a track.** A **media item appears**, backed by our source. The item is **~1 second long — this placeholder length is EXPECTED and CORRECT for 4.1** (the correctly-sized item == real animation duration is **Story 4.2**). Do not be alarmed by the 1 s length. | AC2 |
| 3 | Foreign files not hijacked | Drag a **`.wav`** (and a **`.txt`**) onto a track. Reaper behaves **exactly as normal** — the `.wav` imports as audio as it always would; the `.txt` is handled (or rejected) by Reaper, **not** turned into one of our items. Our factory returned `nullptr` for it. | AC2 |
| 4 | Symmetric deregister, no crash | **Close the project, then unload the extension / quit Reaper.** No crash, no hang, no error dialog. (On unload we deregister `-pcmsrc` with the **same** pointer, leaving no live pointer into our DLL — NFR-R3.) Re-opening Reaper re-registers cleanly (re-check row 1). | AC3 |
| 5 | Same pointer on register/unregister | *(Source audit, dev-box confirmable.)* `plugin_main.cpp` passes `PcmSourceRegistration()` to **both** `Register("pcmsrc", …)` (load) and `Register("-pcmsrc", …)` (unload) — the **identical** address, and `rec->Register` is called **only** from `plugin_main.cpp` (boundary rule). | AC3 |
| 6 | No regression / no scope creep | The viewer still opens via `RAV: Open Viewer` and **renders identically** to Epic 3; a project with **no animation items** behaves exactly as before. **No new `REAPERAPI_WANT_*` symbol**, no asset loaded, no GL touched in this story. | AC4 |
| 7 | Single-DLL / warning-free | `build\Release\` holds only `reaper_animviewer.dll` (no new DLL); build is clean at `/W3 /permissive-`; **no new dependency**; the only changed files are `src/pcm_source_anim.{h,cpp}` (new), `src/plugin_main.cpp`, `CMakeLists.txt` (+1 source line), and this gate doc. | AC4 |

> **Scope note (mirrors the Phase 2 notes):** Story 4.1 delivers **registration + a
> working source object only**. The success signal here is that an item **appears at
> all** on drop and the extension **registers/unregisters without crashing** — **not**
> a correctly-sized item and **not** synced playback. A **~1 s placeholder item** is
> the *correct* result for 4.1. Deliberately deferred to later Epic 4 stories:
> **item length == real animation duration** (FR9, the 2 s load budget) is **Story
> 4.2**; **playhead → animation-frame mapping** (FR10, the transport drive) is
> **Story 4.3**; **per-item `SaveState`/`LoadState` content** (D9) is **Epic 6**.
> If the dropped item is one second long and the viewer does not yet jump frames
> with the playhead, that is **expected** — this story only proves Reaper can *hold*
> an animation as an item.

## 4. Recording the result

Per project convention (`feedback_trust_ingame_validation`): when these checks pass
in real Reaper, **that is the gate** — note the date and the fixtures used here, and
the story moves to done. Registration, item-on-drop, foreign-file pass-through, and
no-crash-on-unload are validator-confirmed on Windows; they are not re-litigated on
the Linux dev box (which can only confirm the source audits — scope, the single
`Register` boundary, the same-pointer register/unregister, no new API symbol — and a
CMake configure that is host-stubbed on non-Windows).

**Result:** Story 4.1 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-26).
Factory registered at load (`[RAV] info: pcmsrc factory registered`); dropping an
animation file creates a media item (~1 s placeholder length, as expected for 4.1);
foreign files (`.txt`/`.wav`) behave Reaper-normal; unload/quit does not crash. The
in-Reaper pass IS the gate (AR19) — correct item length is Story 4.2.

## 5. Story 4.2 — correctly-sized & named item (real animation duration)

Story 4.2 makes the dropped item **the length of the animation** and **named after
the file** — the placeholder length from 4.1 is now only a *fallback*. The duration
comes from a **CPU-only parse** (a new `ProbeAnimationDuration`, no GL, no mesh
processing), so it is correct **whether or not the viewer window is open**. The
click-based test is: **drop a clip whose duration you know → the item's length on the
timeline matches that duration**, and the item is **named after the file**.

Still **out of scope** (do **not** fail 4.2 for these): the viewer does **not** yet
display the dropped asset and does **not** follow the playhead — that is **Story
4.3**. The proof 4.2 works is purely the item's **length** and **name** on the
timeline, visible **with the viewer closed**.

**Suggested fixtures:**
- *Known-duration clip:* a Mixamo clip (read its length in Blender / FBX Review) or a
  Demute fixture whose duration you know — e.g. a ~4 s and a ~1.5 s clip.
- *Clip-less / static:* a static `.glb` with **no** animation (a prop/mesh export).
- *Foreign:* a `.wav` and a `.txt` (must still behave Reaper-normal).

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Item length == real duration | Drop a clip of known length **N** seconds. The created item is **~N s long** (within rounding) — **not** the ~1 s placeholder. A ~4.0 s clip → ~4 s item; a ~1.5 s clip → ~1.5 s item. | AC1 |
| 2 | Correct length with viewer **closed** | Repeat row 1 **without opening the viewer**. The length is still correct — duration is a CPU parse, independent of GL / the viewer window. | AC1 |
| 3 | Item named after the file | The created item/take is **named after the dropped file** (e.g. `Hip Hop Dancing.fbx`), so you can tell which animation it holds at a glance. | AC2 |
| 4 | Clip-less / unparseable → 1 s fallback | Drop a **static `.glb`** (no animation) — or a file that fails to parse. An item still appears at the **1 s fallback length** (never 0-length / degenerate), and Reaper does **not** crash. | AC3 |
| 5 | Foreign files still not hijacked | Drop a **`.wav`** and a **`.txt`**. Reaper behaves **exactly as normal** — our factory still returns `nullptr` for them (unchanged from 4.1). | AC5 |
| 6 | Fast / well under budget | The drop-to-item is **near-instant** — the duration probe is well under the 2 s load budget (it does only the cheap CPU parse, `0` post-process flags). | AC4 |
| 7 | No regression / no scope creep | The viewer still opens via `RAV: Open Viewer` and renders identically to Epic 3; register/unregister is still symmetric (`-pcmsrc`, same pointer); unload/quit does not crash. **No new `REAPERAPI_WANT_*`, no new dependency.** Build clean at `/W3 /permissive-`. | AC5 |

> **Scope note:** 4.2 delivers a **correctly-sized + named** item — nothing more. The
> viewer showing the asset and **playhead → displayed frame** (FR10) is **Story 4.3**;
> per-item `SaveState`/`LoadState` content (D9) is **Epic 6**. If the item is the right
> length and name but the viewer does not yet jump frames with the playhead, that is
> **expected**.

**Result:** Story 4.2 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-26).
Dropping a known-duration animation creates a correctly-sized item (timeline length ==
real clip duration, confirmed **with the viewer closed**), named after the file; a
clip-less / static `.glb` falls back to the 1 s placeholder (no 0-length, no crash);
`.txt`/`.wav` behave Reaper-normal. The in-Reaper pass IS the gate (AR19) — display +
playhead → frame is Story 4.3.

## 6. Story 4.3 — playhead position drives the displayed animation frame

Story 4.3 wires the whole chain together: the **Reaper transport drives both which
asset the viewer shows and which frame of it**. Open the viewer, drop an animation,
and **press Play / scrub the cursor** — the rig moves with the transport,
frame-accurate, and **holds at the ends**. The displayed time is `animTime =
playheadTime − itemStart`, clamped to `[0, itemLength]`, recomputed **every frame,
never stored** — there is no internal free-running clock once an item is driving the
view. The displayed asset is the one **loaded from the source file of the RAV item
spanning the playhead** (loaded on the render thread, where the GL context is
current), so scrubbing onto a different item **switches the asset**.

Still **out of scope** (do **not** fail 4.3 for these): **overlap / track-priority**
among multiple items (first RAV item spanning the playhead wins — that is **Story
4.5**); native **move / resize / color / rename + audio/MIDI coexistence**
validation (**Story 4.4**, though most works for free now); per-item
`SaveState`/`LoadState` content (**Epic 6**); any browser/preview (**Epic 5**).

**Suggested fixtures:**
- *Animated clip(s):* a Mixamo / Demute clip of known duration (e.g. a ~4 s and a
  ~1.5 s clip) — ideally **two different** files to test the asset switch.
- *Static:* a static `.glb` with no animation (shows its single pose, must not crash).
- *Foreign:* a `.wav` and a `.txt` (must still behave Reaper-normal).

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Play drives the frame | Drop an animation, open the viewer, **press Play**. The rig **animates in time with the transport** as the play cursor advances, smoothly (full window frame rate, continuous audio clock), and **loops with Reaper's loop**. | AC1 |
| 2 | Scrub drives the frame | **Stop**, then **scrub the edit cursor** back and forth across the item. The displayed pose **tracks the cursor frame-accurately** (no perceptible lag — within ~one rendered frame, ≈16 ms). | AC1, AC4 |
| 3 | Displayed asset = item under playhead | The viewer shows the **asset of the item the playhead is over** — not the dialog-picked fixture. (The startup fixture was just the no-item placeholder.) | AC2 |
| 4 | Scrub onto a 2nd item switches the asset | Drop a **second, different** animation on another track. Scrub the cursor from the first item **onto the second** → the **displayed asset switches** to the second file (`now showing …` in the console). | AC2 |
| 5 | Hold past the end | Scrub / play **past the end** of the item. The rig **holds the last frame** — it does **not** disappear, flicker to garbage, jump, or crash. | AC3 |
| 6 | Hold before the start | Move the playhead **before the item's start**. The rig **holds the first frame** (same — no disappear/flicker/crash). | AC3 |
| 7 | Static asset still renders | A **static `.glb`** item (no animation) under the playhead shows its **single pose** (it just doesn't move with the transport). | AC5 |
| 8 | Foreign files still not hijacked | `.txt` / `.wav` still behave **Reaper-normal** (no item hijacked, no viewer effect). | AC5 |
| 9 | No regression / scope-clean | Viewer still opens via `RAV: Open Viewer` and docks; camera orbit/zoom/pan/Reset still work; item **length + name** from 4.2 unchanged; register/unregister still symmetric (`-pcmsrc`, same pointer — `plugin_main.cpp` untouched); unload/quit does not crash. **No new dependency**; the only new symbols are the 8 transport/item `REAPERAPI_WANT_*`. Build clean at `/W3 /permissive-`. | AC5 |

> **Scope note:** 4.3 makes **one** item under the playhead drive **which asset +
> which frame** — that is the whole proof. **Overlap / priority** among multiple
> items is **Story 4.5** (here, first match wins); native **move/resize** validation
> is **Story 4.4**; `SaveState`/`LoadState` content is **Epic 6**. If a single item
> plays/scrubs/holds and a second item switches the asset, 4.3 passes — even if
> overlapping items don't yet pick by priority.

**Result:** Story 4.3 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-27).
The playhead drives both the displayed asset and the frame: play/scrub move the rig
with the transport, the ends hold, and scrubbing onto a second item switches the
asset. The in-Reaper pass IS the gate (AR19): transport timing, scrub feel, and the
hold-at-ends are only observable in-Reaper on Windows. On Linux only the CMake
configure + the source/scope audit were checkable (the DLL/GL/transport link is
host-stubbed, as in every prior Phase-2/3 story).
