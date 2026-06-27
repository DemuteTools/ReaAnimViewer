# Phase 4 — Validator Gate

Step-by-step acceptance test for **Phase 4** (Epic 6 — **save / recall Reaper
sessions**: per-item state, panel/viewport state, and cross-machine portability all
survive a project save/reopen). Written for Antho (the validator) to run on Windows.
This file is the **sole authority on Phase 4 completion** (AR19) and grows as Epic 6
progresses: **Story 6.1** seeds the rows below (per-item binding round-trips through
`SaveState`/`LoadState` — D9); Stories 6.2 (panel/viewport dock state) and 6.3
(cross-machine native portability) append their own.

**You are testing the right thing if**, for Story 6.1, you can **drop 2–3 animation
items, Save, close, and reopen** the project and find **every item back, bound to its
file, and playing under the playhead with no manual action** — no empty / broken /
"offline media" item, no re-drop — and saving **never corrupts the `.rpp` or touches
any other track**.

---

## Prerequisites

- Reaper 7.x (`caller_version == 0x20E`, current SDK targets 7.72)
- Visual Studio 2022 with **Desktop development with C++**
- CMake ≥ 3.20 and Git for Windows on PATH
- The same assimp 6.0.5 + GLM 1.0.3 + stb vendored stack as Phases 1–3 — **no new
  dependency** in this story (no new importer, **no new API symbol**). First configure
  is slow; later builds reuse the cache.

> Phase 4 adds **no** new viewport, panel, shader, renderer, GL-resource, loader,
> `reaper_api.h`, `plugin_main.cpp`, or `CMakeLists.txt` change for Story 6.1. The
> **only** production code is the two `PCM_source` virtuals `SaveState` / `LoadState`
> (+ a small `key=value` helper) in `src/pcm_source_anim.cpp` — they are virtuals
> **Reaper calls** during project save/load, **not** a new registration, so the
> boundary rule is preserved (`-pcmsrc` still deregisters with the same pointer). Every
> Epic 1–4 behavior renders and behaves identically.

## 1. Build & install

Identical to the Phase 1 / 2 / 3 gate (§1):

```cmd
cd \path\to\ReaAnimViewer
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
copy build\Release\reaper_animviewer.dll "%APPDATA%\REAPER\UserPlugins\"
```

**Expected:** **zero MSVC warnings at `/W3 /permissive-`** for our own sources, and a
single DLL at `build\Release\reaper_animviewer.dll` with **no `assimp*.dll`** beside it
(assimp is statically linked; stb is header-only). Close and reopen Reaper to re-scan.

## 2. What you are testing

Story 6.1 turns the empty `SaveState` / `LoadState` stubs into a real, **versioned**
round-trip so **per-item state survives save/reopen on the same machine**. The file
**path** rides Reaper's native `FILE "…"` / `GetFileName` / `SetFileName` mechanism
(so the binding likely survives even with the old stub — this story **verifies** that
and adds a **defensive** `file=` fallback applied **only if the native path is still
empty**, so a relinked project always keeps the native path — Story 6.3 stays native).
On top of the path, `SaveState` writes a `rav_ver=1` schema marker; `LoadState`
**ignores any line it doesn't recognize** (the native uppercase `FILE` line, a nested
`<…>` block, or any unknown future key) and **never hard-fails** on a bad line.

The click-based test is: **drop → Save → close → reopen → confirm every item rebinds
and plays**; **inspect the saved `.rpp`** in a text editor; **stress data-safety** with
a mixed audio/video/MIDI + RAV project; and **hand-add a bogus key** to confirm
forward-compat. Restoration **rides the existing 4.3–4.5 poll/load pipeline** — once a
source reappears with its path, the viewer's `GetCurrentAnimItem` poll finds it, lazily
loads its asset, and the playhead drives the frame, all unchanged.

**Suggested fixtures:**
- *2–3 different animation clips:* e.g. a Mixamo / Demute `.glb` or `.fbx` of known
  duration and a second **visibly different** clip — placed on **one or more tracks at
  different positions** (and optionally a third, to confirm several items round-trip).
- *Mixed media for the no-data-loss row:* an audio `.wav`, a video (`.mp4`/`.mov`), and
  a MIDI item on other tracks.
- *A static `.glb`* (no animation) is fine as one of the items — it should round-trip and
  show its single pose just as it did before save.

## 3. Story 6.1 — per-item binding survives save / reopen (`SaveState`/`LoadState`)

Story 6.1 makes the **animation file binding and our versioned per-item line set**
survive a project save/reopen. **It does not** persist per-item **camera** framing
(global today — deferred), **`time_offset`/`time_scale`** (native take
`D_STARTOFFS`/`D_PLAYRATE` — not ours), or **panel/dock** state (Story 6.2). The proof
it works is simply that **every dropped item comes back bound and playable** after a
close/reopen, with **no manual action and no data loss**.

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Binding survives same-machine save / reopen | Drop **2–3** RAV items (different files / positions / tracks). **Save**, **close** the project, then **reopen** it. **Every item is back**, bound to its file, and **plays under the playhead** (move the cursor over each) with **no manual action** — no empty item, no broken/"offline media" item, no re-drop. | AC1, AC2 |
| 2 | Chunk inspection | Open the saved `.rpp` in a text editor and find each `<SOURCE RAV_ANIM …>` chunk. It carries the path (Reaper's native `FILE "…"` line **and/or** our `file=` line) plus our `rav_ver=1` line. **Nothing outside our source chunks changed** — other tracks/items are as they were. | AC1, AC4 |
| 3 | No data loss (mixed project) | Build a project with **audio + video + MIDI + RAV** items, **Save / close / reopen**. **No track dropped, no item lost, no corruption** — the non-RAV media is byte-identical and plays exactly as before (NFR-R2). | AC4 |
| 4 | Forward-compat (unknown key ignored) | Hand-edit the saved `.rpp` to add a bogus `future_key=xyz` line **inside** a `<SOURCE RAV_ANIM …>` chunk (and optionally a malformed `garbage no equals` line). **Reopen** → the item **still loads and plays** — the unknown/malformed line is ignored, the item is not dropped, no error dialog. | AC3 |
| 5 | No regression / scope-clean | 4.1–4.5 behavior intact: drop → **sized + named** item (4.2); **play / scrub / hold** at the ends (4.3); native **move / resize both edges / left-trim-reveals-later-frames / color / rename** + audio/MIDI/video **coexistence** (4.4); **overlap → topmost-track** selection + ≥10 items @ ≥60 fps (4.5). Register/unregister still **symmetric** (`-pcmsrc`, **same pointer** — `plugin_main.cpp` **untouched**); unload / quit does **not** crash. **No new `REAPERAPI_WANT_*`, no new dependency**; build clean at `/W3 /permissive-`; the **only** changed file in `src/` is `pcm_source_anim.cpp`. | AC5 |

> **Scope note (mirrors the Phase 3 notes):** Story 6.1 makes **per-item binding +
> our versioned line set** survive save/reopen **on the same machine** — nothing more.
> Deliberately **out of scope** (do **not** fail 6.1 for these): per-item **camera
> framing** is **not** persisted (the camera is **global** today — renderer-owned
> `OrbitCamera`, reset on `SetAsset`; deferred with a concrete trigger in
> `deferred-work.md`); **`time_offset`** (take `D_STARTOFFS`) and **`time_scale`** (take
> `D_PLAYRATE`) are **native take state** Reaper persists — **not** written by us;
> **panel / viewport dock** state is **Story 6.2**; **cross-machine relink** (move the
> project to another machine) is **Story 6.3**. If every item rebinds and plays after a
> same-machine reopen, the `.rpp` round-trips with no data loss, and a bogus key is
> ignored, 6.1 passes — even though the camera framing isn't restored per-item and a
> cross-machine move isn't verified here.

## 4. Recording the result

Per project convention (`feedback_trust_ingame_validation`): when these checks pass in
real Reaper, **that is the gate** — note the date and the fixtures used here, and the
story moves to done. Binding survival, no-data-loss, and forward-compat are only
observable **in-Reaper on Windows**; they are not re-litigated on the Linux dev box,
which can only confirm the source/scope audit (one `.cpp` changed, no new symbol, no
registration, the `-pcmsrc` same-pointer boundary, `CMakeLists` unchanged), an isolated
`-Werror` compile of the `key=value` splitter + `SaveState`/`LoadState` bodies (the
`only-if-empty` guard, the ignore-unknown / return-`0` contract — all asserted), and a
CMake configure that is host-stubbed on non-Windows.

**Result:** Story 6.1 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-27).

Antho ran §1 in real Reaper on Windows: 2–3 RAV animation items dropped across tracks/positions survive a Save → close → reopen — every item comes back bound to its file and plays under the playhead with no manual action, no empty/broken/"offline media" item. The saved `.rpp` carries the path inside our `<SOURCE RAV_ANIM …>` chunk (native `FILE` and/or our `file=` + `rav_ver=1`) with nothing outside our chunks changed; a mixed audio/video/MIDI + RAV project round-trips with no track or data loss (NFR-R2); a hand-added bogus `future_key=` line is ignored and the item still loads (AC3 forward-compat); and the Epic 1–4 behaviors plus the symmetric `-pcmsrc` boundary are intact. AC1–AC5 confirmed in-Reaper — gate passed.
