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
