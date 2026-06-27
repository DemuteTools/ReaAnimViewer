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

---

## 5. Story 6.2 — panel/viewport dock state survives save / reopen + Reaper restart

Story 6.2 makes the **viewer panel come back where you left it** so reopening a session
doesn't make you re-dock and re-frame the viewport (FR32, reformulated 2026-06-27 off
ReaImGui → **native** docker). The crux: **a docked native GL window's dock position is a
Reaper-*global* UI concern** — Reaper stores it in `reaper.ini`, keyed by the window's
`identstr`, **not** per-project. The viewport is already handed to the docker as
`DockWindowAddEx(g_hwnd, kDockName, kDockIdent, true)` with a **stable**
`kDockIdent = "ReaAnimViewer.Viewer"` ([viewer_window.cpp:599–602](../src/viewer_window.cpp#L599)),
and the existing code comment already states *"identstr persists the dock position across
sessions."* So this is a **verify-first (AR20), expected-zero-code** story — exactly like
6.1/6.3: we do **not** invent serialization. Task 0 below confirms whether the native
docker already round-trips the dock position. **If it does, 6.2 is zero-code on the dock
position** and the native behavior is documented here as the PASS. **Only if** an in-Reaper
gap is found (panel does *not* return to its remembered dock) is a minimal **global**
`SetExtState`/`GetExtState` fallback added in `viewer_window.cpp` — **never** ReaImGui,
**never** a custom screenset engine.

> Phase 4 adds **no** new viewport, panel, shader, renderer, GL-resource, loader,
> `reaper_api.h`, `plugin_main.cpp`, or `CMakeLists.txt` change for Story 6.2 **in the
> expected (native-already-works) path** — it is **docs-only**. The existing
> `DockWindowAddEx` ↔ `DockWindowRemove` symmetric lifecycle and `UnregisterViewerClass`
> on close/unload are preserved **unchanged** (NFR-R3 clean unload / AR15). `pcm_source_anim.cpp`
> is **off-limits** for this story — that is the per-*item* surface (§3, Story 6.1), not
> the panel.

**You are testing the right thing if** you can **dock the viewer at a deliberate,
non-default spot** (e.g. the right docker, a specific tab), **Save / close / reopen** the
project — *and then quit and relaunch Reaper* — and find the **panel returns to that same
dock position** when you open it again, with **no GL/window leak** and Reaper unaffected.

### Check rows (click-based, in-Reaper on Windows)

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | Dock survives project reopen (same Reaper session) | Open the viewer (`RAV: Open Viewer`); **drag it to a deliberate, non-default dock position** (e.g. the right docker, or a specific docker tab). **Save** the project, **close** it, and **reopen** it. Open the viewer → the panel is at the **same dock position** (within one Reaper session the global docked panel typically isn't disturbed at all — that is a PASS). | AC1, AC3 |
| 2 | Dock survives a **Reaper restart** (the `reaper.ini` path) | With the viewer docked at that position, **fully quit Reaper and relaunch it**, reopen the project, and open the viewer → the dock position is **still remembered**. This is the `kDockIdent`/`reaper.ini` path — the most likely place native persistence actually lives, since dock position is **global**, not per-project. | AC1, AC3 |
| 3 | No host block / no resource leak | Opening and closing the viewer repeatedly (toggle action), docking/undocking, and quitting Reaper **never hang the host** and leave **no leaked GL context / window class / dock registration** — the `DockWindowAddEx`↔`DockWindowRemove` + `UnregisterViewerClass` lifecycle stays intact; unload/quit does not crash. | AC2 |
| 4 | No regression / scope-clean (expected docs-only path) | Epic 1–4 behavior intact: viewer docks/floats, renders ≥60 fps, close/reopen via the toggle action works, docker-tab-X hide then re-open works (the window is **never destroyed on hide**, only on toggle/unload). In the expected native-works path, **no `src/` file changed** — `git diff` touches only docs + `sprint-status.yaml`; no new `REAPERAPI_WANT_*`, no new dependency, no `CMakeLists.txt` change. | AC2 |

> **Scope note (mirrors §3):** Story 6.2 makes the **panel dock position** come back —
> nothing more. Deliberately **out of scope** (do **not** fail 6.2 for these): **per-item
> camera framing** is **not** persisted (the camera is **global** today — renderer-owned
> `OrbitCamera`, reset on `SetAsset`; deferred, see `deferred-work.md`); **`time_offset`**
> (take `D_STARTOFFS`) and **`time_scale`** (take `D_PLAYRATE`) are **native take state**;
> **cross-machine relink** is **Story 6.3**; a **ReaImGui** panel is **post-MVP** (Epic 5,
> postponed). If the docked panel returns to its remembered position after a save/reopen
> and a Reaper restart, with no host block or resource leak, **6.2 passes** — even though
> camera framing isn't restored and a cross-machine move isn't verified here.

### Recording the result (Story 6.2)

Per project convention (`feedback_trust_ingame_validation`): when these checks pass in real
Reaper, **that is the gate** — note the date and the dock position used here, and the story
moves to done. Dock-position persistence is only observable **in-Reaper on Windows**; the
Linux dev box can only confirm the source/scope audit (no `src/` change in the expected
path, the `DockWindowAddEx`↔`DockWindowRemove`/`UnregisterViewerClass` lifecycle unchanged,
no new symbol/registration, `CMakeLists` unchanged) and a host-stubbed CMake configure.

> **If row 1 or 2 reveals a real gap** (the panel does **not** return to its remembered
> dock), that is the trigger for **Task 1** in the story: a minimal **global**
> `SetExtState`/`GetExtState` fallback in `viewer_window.cpp` (+ the `WANT_` declarations in
> `reaper_api.h`), behind the existing AR18 no-throw boundary — **never** ReaImGui, **never**
> a custom screenset engine. Re-run §5 after the fix.

**Result:** Story 6.2 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-27).

Antho ran §5 in real Reaper on Windows: the docked viewer returns to its remembered dock
position across a project save → close → reopen **and** across a full Reaper quit/relaunch —
confirming the verify-first hypothesis that the native docker (`DockWindowAddEx` + stable
`kDockIdent`, Reaper-global `reaper.ini`, not per-project per D9) **already round-trips the
dock position with zero code**. No host block and no leaked GL context / window class / dock
registration (the `DockWindowAddEx`↔`DockWindowRemove` + `UnregisterViewerClass` lifecycle
intact); Epic 1–4 behavior unaffected. The expected-zero-code path held — **no `src/` change**,
the global `SetExtState`/`GetExtState` fallback (Task 1) was **not** needed. AC1–AC3 confirmed
in-Reaper — gate passed.

---

## 6. Story 6.3 — cross-machine portability via Reaper's native media copy/relink

Story 6.3 makes a session **find its animations on another machine (or after the project
folder is moved) without re-importing gigabytes** (FR46, reformulated 2026-06-27 **off** a
home-grown relative-path/missing-media remap onto **native** Reaper media handling). The
crux: **cross-machine portability is delivered by Reaper's native "copy media into project
directory" + relink, and our `PCM_source` already participates because it is file-backed.**
Reaper writes a native `FILE "…"` line for **any** source whose `GetFileName()` returns a
non-empty path — ours does ([src/pcm_source_anim.cpp:93](../src/pcm_source_anim.cpp#L93)) —
so our items are **eligible** for copy-into-project. On reopen Reaper recreates the source
via the registered `CreateFromType("RAV_ANIM")`
([src/pcm_source_anim.cpp:175-181](../src/pcm_source_anim.cpp#L175)) and then `SetFileName`s
the (copied/relinked) path, which re-probes the clip
([src/pcm_source_anim.cpp:95-100](../src/pcm_source_anim.cpp#L95)). This is the **same native
mechanism Story 6.1 verified for the same-machine round-trip** — 6.3 just exercises it across
a **moved folder / second machine**. The `file=` defensive fallback stays **only-if-empty**
([src/pcm_source_anim.cpp:160-163](../src/pcm_source_anim.cpp#L160)): on a relinked project
Reaper's native `SetFileName` sets `m_path` first, so our stale `file=` is **skipped** and the
native/relinked path is **always authoritative** (the 6.3-safety guarantee 6.1 built in).

→ This is therefore a **verify-first (AR20), expected-zero-code** story — exactly like
6.1/6.2: we do **not** build a remap engine. The only sanctioned code path is the
**only-if-gap** Task 2 ("make the source participate in the *native* mechanism"), never our
own portability layer. The **missing-media diagnostic already exists** and is **not**
rebuilt: `LoadAsset` opens with a `std::filesystem::exists` check
([src/asset_loader.cpp:745](../src/asset_loader.cpp#L745)); the transport poll logs
`LogError("load failed [%s]: %s", …)` on failure and **advances the path gate whether the
load succeeded or failed** ([src/viewer_window.cpp:198-214](../src/viewer_window.cpp#L198)),
so an unresolved file does not wedge the loop and every *other* item still loads on its own
poll — per-item failure isolation (AR17 / FR37).

> Phase 4 adds **no** new remap engine, search-path resolver, "locate missing media" dialog,
> or portability layer of our own for Story 6.3 **in the expected (native-already-works)
> path** — it is **docs-only**. The `GetFileName`/`SetFileName` native relink contract, the
> `only-if-empty` `file=` guard, the `CreateFromType("RAV_ANIM")` tag, and the
> transport-driven poll/load pipeline are all preserved **unchanged**. `pcm_source_anim.cpp`
> is touched **only if** an in-Reaper gap is found (Task 2) — and even then only a minimal
> native-participation fix (`GetFileName`/`IsAvailable` reporting), **never** a remap engine.

**You are testing the right thing if** you can **save a project with "copy media into project
directory"**, **move the whole project folder** (or copy it to a second PC), **reopen** it,
and find that **every RAV item relinks and plays under the playhead with no forced
re-import** — and that a **deliberately unresolved** path surfaces a **console diagnostic**
while **the rest of the session keeps working**.

### Check rows (click-based, in-Reaper on Windows)

| # | Check | Pass criterion | AC |
|---|---|---|---|
| 1 | "Copy media into project directory" embeds our items | Build a project with **2–3 RAV items** (`.glb`/`.fbx`) referencing files **outside** the project folder. **Save** with **"copy media into project directory"** (Save As… → copy media, or File → Save project / clean up → copy). → our animation files are **copied into the project's media folder** and the `.rpp` references the **copied** path inside our `<SOURCE RAV_ANIM …>` chunk. | AC1, AC2 |
| 2 | Moved-folder / second-machine reopen relinks + plays | **Move the entire project folder** to a new location (simulating another machine — or copy it to a second PC), then **reopen** the `.rpp`. → **every item relinks** to the in-project copy and **plays** under the playhead, with **no forced media re-import** and **no manual relink** prompt. The native `SetFileName` path wins; our `file=` fallback is skipped (`m_path` already set). | AC1, AC2 |
| 3 | Unresolved path → console diagnostic + per-item isolation | On the moved project, **delete or withhold one** referenced media file, reopen, and **move the playhead over that one item**. → a **console diagnostic** names the path/category (`load failed [file-not-found]: …` via `LoadAsset`→`LogError`, AR16 — never a popup) **and every other item still rebinds, loads, and plays** (the poll advances the gate on failure, AR17 — the bad item never wedges the loop or retries every frame). | AC3 |
| 4 | No regression / scope-clean (expected docs-only path) | Epic 1–6 behavior intact: same-machine save/reopen still rebinds (Story 6.1, §1), the docked panel still returns to its dock (Story 6.2, §5), drop→display→transport→multi-item-priority all work. In the expected native-works path, **no `src/` file changed** — `git diff` touches only docs + `sprint-status.yaml`; no new `REAPERAPI_WANT_*`, no new dependency, no `CMakeLists.txt` change; the symmetric `-pcmsrc` boundary and the `only-if-empty` `file=` guard are intact. | AC1–AC3 |

> **Scope note (mirrors §3/§5):** Story 6.3 makes a session **portable across machines via
> Reaper's native copy/relink** — nothing more. Deliberately **out of scope** (do **not**
> fail 6.3 for these, and do **not** build them): a **custom relative-path / missing-media
> remap engine**, a search-path resolver, or a **"locate missing media" dialog** of our own
> (FR46 was reformulated **off** a home-grown remap onto native copy/relink — the defining
> boundary of this story); **per-item camera framing** (the camera is **global** today,
> reset on `SetAsset`; reaffirmed-deferred from 6.1/6.2); **`time_offset`/`time_scale`**
> (native take `D_STARTOFFS`/`D_PLAYRATE`, which Reaper persists/relocates itself); **panel/
> dock** state (= Story 6.2, done); **reload of changed-on-disk files, graceful-degradation
> hardening, FBX/Collada coverage** (= Epic 8, post-release). If a project saved with "copy
> media" relinks and plays on a moved folder, and an unresolved path logs a diagnostic while
> the rest keeps playing, **6.3 passes** — even though no custom portability layer exists.

### Recording the result (Story 6.3)

Per project convention (`feedback_trust_ingame_validation`): when these checks pass in real
Reaper, **that is the gate** — note the date, then 6.3 moves to done and **Epic 6 can close**
(6.1/6.2/6.3 all done). Cross-machine relink, native copy-media, and the moved-folder reopen
are only observable **in-Reaper on Windows**; the Linux dev box can only confirm the
source/scope audit (no remap engine added, the `only-if-empty` guard intact, at most
`pcm_source_anim.cpp` touched, no new symbol/registration, `CMakeLists` unchanged) and a
host-stubbed CMake configure.

> **If row 2 reveals a real gap** (a moved-folder reopen does **not** relink + play our
> items), that is the trigger for **Task 2** in the story: diagnose *which* native step
> failed — (a) "copy media" did **not** copy our files, or (b) relink did **not** call
> `SetFileName` on the moved path — and apply the **minimal** native-participation fix for the
> proven gap only (e.g. ensure `GetFileName`/`IsAvailable` report so Reaper treats the source
> as copyable/relinkable), in `src/pcm_source_anim.cpp`, behind the existing AR18 no-throw
> boundary. **Never** invent path rewriting, a search-path engine, or a remap dialog. If row 3
> shows the diagnostic is unclear or an unresolved item disturbs the others, that becomes a
> review follow-up scoped to the existing `LoadAsset`/`viewer_window` path — still no remap
> engine. Re-run §6 after any fix.

**Result:** Story 6.3 — **PASS** (Antho, in-Reaper Windows validation, 2026-06-27).

Antho ran §6 in real Reaper on Windows: a project saved with "copy media into project
directory" embeds our RAV items, the whole project folder is moved (second-machine
simulation) and reopened — **every item relinks and plays under the playhead with no forced
re-import** — and an unresolved path surfaces a console diagnostic while the rest of the
session keeps playing. This confirms the verify-first hypothesis that cross-machine
portability is delivered by Reaper's **native** copy-into-project + relink and our file-backed
`PCM_source` already participates (`GetFileName`/`SetFileName`/`CreateFromType`), the
`only-if-empty` `file=` guard keeping the native/relinked path authoritative. The
expected-zero-code path held — **no `src/` change**; the only-if-gap native-participation fix
(Task 2) was **not** needed. AC1–AC3 confirmed in-Reaper — gate passed. **Epic 6 closes**
(6.1/6.2/6.3 all done).
