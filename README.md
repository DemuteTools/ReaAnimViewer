<img width="2116" height="957" alt="image" src="https://github.com/user-attachments/assets/46506343-093d-42c8-ba8e-0e8efb70d41e" />

# DM ReaAnimViewer

3D animation viewer for REAPER. Load glTF and FBX animations straight onto your timeline and watch the animated character play in sync with the REAPER playhead, from any camera angle.

## Why Use This Tool?

Sound designing for game characters usually means screen-recording every animation in the engine, importing the videos into REAPER, and redoing all of it every time an animator changes a few frames. Once recorded, the camera angle is frozen: if the gesture you need to time is hidden behind the character, you're back to the engine. ReaAnimViewer removes the video step entirely.

- **No more screen captures:** Drop the animation file itself on a track. The rig is rendered live, frame-accurate, with no video file, no stutter, no re-recording.
- **Move the camera while you design:** Orbit, pan and zoom around the character at any time, and snap to front/side/top views with the navigation cube. See the foot contact or the hand swing the recorded video would have hidden.
- **Render the video with your sound:** Frame camera shots in the viewer and render picture + audio with REAPER's own Render dialog or Region Render Matrix. No screen capture, no intermediate video file.
- **Behaves like native REAPER media:** Animation items can be moved, trimmed, looped, saved with the project and copied into the project folder like any audio item, so your session and your sounds stay in sync.

## How It Works

ReaAnimViewer is a native REAPER extension (a `.dll`, plus a small video FX that puts the animation in your rendered videos). It teaches REAPER to read `.glb`, `.gltf` and `.fbx` files as media: drop one on a track and it becomes an item whose length matches the animation. A dockable viewer window renders the animated mesh at the current playhead position: play, stop, scrub or loop and the character follows.

- **Engine agnostic:** Works with any animation exported to glTF or FBX from Unreal, Unity, Godot, Blender, Maya or a proprietary engine.
- **Skinned meshes and textures:** Full skeletal deformation, diffuse/normal/specular maps, multi-material meshes.
- **Several animations per project:** Put different animations on different tracks; the viewer shows the one on the topmost track under the playhead.
- **Viewer tools:** A built-in side menu for lighting, floor, shadows and render quality.
- **Everything bundled:** No ReaImGui, no SWS, no runtime to install. The whole viewer is a single extension file; the video FX comes in the same package.

---

## Table of Contents

- [Installation](#installation)
  - [Install with the Demute Reaper Toolkit (easiest)](#install-with-the-demute-reaper-toolkit-easiest)
  - [Install with ReaPack](#install-with-reapack)
  - [Updating](#updating)
  - [Upgrading from 0.1.x](#upgrading-from-01x)
  - [Uninstalling](#uninstalling)
  - [Manual Installation](#manual-installation)
  - [Troubleshooting](#troubleshooting)
- [Getting Started](#getting-started)
- [Supported Files](#supported-files)
- [Camera Controls](#camera-controls)
- [Model / Skeleton](#model--skeleton)
- [Viewer Menu](#viewer-menu)
  - [Light](#light)
  - [Ground](#ground)
  - [Shadow](#shadow)
  - [Performance](#performance)
- [Shortcuts](#shortcuts)
- [Working with Animation Items](#working-with-animation-items)
- [Rendering Video](#rendering-video)
  - [Adding the video FX](#adding-the-video-fx)
  - [Video view](#video-view)
  - [Shots](#shots)
  - [Output](#output)
  - [Rendering](#rendering)
  - [REAPER catch-up while playing](#reaper-catch-up-while-playing)
  - [If the video FX shows nothing](#if-the-video-fx-shows-nothing)
- [Auto-Tagging (Event Markers)](#auto-tagging-event-markers)
  - [Quick start: footsteps](#quick-start-footsteps)
  - [Smart Detection](#smart-detection)
  - [Tagging your own events](#tagging-your-own-events)
  - [Rules](#rules)
  - [Presets](#presets)
  - [Roles](#roles)
  - [The Tagging view](#the-tagging-view)
  - [Correcting events](#correcting-events)
  - [Markers](#markers)
  - [Markers follow their item](#markers-follow-their-item)
  - [Editing the markers in REAPER](#editing-the-markers-in-reaper)
  - [Spreading your sounds onto the markers](#spreading-your-sounds-onto-the-markers)
  - [Syncing placed sounds with the Library](#syncing-placed-sounds-with-the-library)
  - [Copies of one animation share their tagging](#copies-of-one-animation-share-their-tagging)
  - [Known limits](#known-limits)
- [Saving and Sharing Projects](#saving-and-sharing-projects)
- [Exporting Animations from Your Engine](#exporting-animations-from-your-engine)
- [Building from Source](#building-from-source)
- [License](#license)

---

## Installation

**Requirements:**

- REAPER 7.x, **64-bit, Windows**
- A graphics card supporting OpenGL 3.3 (any GPU from the last ~10 years)
- [ReaPack](https://reapack.com/) (for the Toolkit and ReaPack installs)

**Nothing else to install.** Everything the viewer needs (3D loader, UI, image decoders) is compiled inside the extension.

Already using version 0.1.x? Read [Upgrading from 0.1.x](#upgrading-from-01x) first.

### Install with the Demute Reaper Toolkit (easiest)

The [Demute Reaper Toolkit](https://www.demute.studio/documentation/reaper-toolkit) is our tool browser for REAPER: it lists every Demute tool and installs or updates them in one click. If you already use it, skip to step 3.

1. Install [ReaPack](https://reapack.com/) if you don't have it already, then restart REAPER.
2. Install the Toolkit (one-time setup):
   1. Go to **Extensions > ReaPack > Import repositories...**, paste the following URL and click **OK**:

      ```
      https://raw.githubusercontent.com/DemuteTools/DM_ReaperToolkit/refs/heads/main/index.xml
      ```

   2. Go to **Extensions > ReaPack > Manage repositories**, select **Demute_Toolkit** and click **Browse packages**.
   3. Search for **DM_ReaperToolkit**, right-click it, choose **Install**, then click **Apply**.
3. Open the Toolkit: **Actions > Show action list**, search for **DM_ReaperToolkit** and run it. It automatically scans for available tools and updates.
4. Select the **ReaAnimViewer** card and install it.
5. Click **Run** on the card. A message confirms that ReaAnimViewer is installed.
6. **Restart REAPER.** Extensions are only loaded at startup.
7. Open the viewer with the action **RAV: Viewer - Open viewer**, or with **Run** on the card.

You only need step 5 once. The Toolkit downloads scripts, while ReaAnimViewer is a native extension that REAPER loads from its `UserPlugins` folder: the first **Run** copies it there. From then on, the extension updates itself.

### Install with ReaPack

1. Install [ReaPack](https://reapack.com/) if you don't have it already, then restart REAPER.
2. In REAPER, go to **Extensions > ReaPack > Import repositories...**
3. Paste the following URL and click **OK**:

   ```
   https://github.com/DemuteTools/ReaAnimViewer/raw/main/index.xml
   ```

4. Go to **Extensions > ReaPack > Browse packages...**, search for **ReaAnimViewer**.
5. Right-click **ReaAnimViewer**, choose **Install**, then click **Apply**.
6. **Restart REAPER.** Extensions are only loaded at startup.
7. Open the viewer with the action **RAV: Viewer - Open viewer** (**Actions > Show action list**, search for **RAV**).

The package also installs a small **RAV_Launcher** script. It is used by the Toolkit; with ReaPack you don't need it (running it just opens the viewer).

### Updating

**With the Demute Reaper Toolkit:** open the Toolkit, click **Update** on the **ReaAnimViewer** card, then restart REAPER. The extension swaps itself for the new version when REAPER closes, so the new version runs after the restart. Nothing else to run.

**With ReaPack:** go to **Extensions > ReaPack > Synchronize packages** (or wait for the automatic check), click **Apply**, then **restart REAPER** so the new version is loaded.

**Manual install:** download the new DLL and video FX and replace the old ones, as in [Manual Installation](#manual-installation).

### Upgrading from 0.1.x

Version 0.1.x came as two ReaPack packages, also when you installed it through the Toolkit. Remove the old one once:

1. Go to **Extensions > ReaPack > Browse packages...** and search for **ReaAnimViewer**.
2. Two entries are listed. Right-click the one whose category is **Extensions**, choose **Uninstall**, then click **Apply**. Keep the one in **Scripts**.
3. Restart REAPER, then run **Extensions > ReaPack > Synchronize packages** and click **Apply**.
4. Restart REAPER again.

Until you do this, ReaPack reports a conflict on `reaper_animviewer.dll` and ReaAnimViewer stays on 0.1.x.

### Uninstalling

**With ReaPack:** go to **Extensions > ReaPack > Browse packages...**, right-click **ReaAnimViewer**, choose **Uninstall**, click **Apply** and restart REAPER. Don't delete the DLL by hand instead: ReaPack would still consider it installed (see [Troubleshooting](#troubleshooting)).

**With the Demute Reaper Toolkit:** uninstall the **ReaAnimViewer** card in the Toolkit, then close REAPER and delete `reaper_animviewer.dll` from the `UserPlugins` folder and `rav_video_fx.clap` from `UserPlugins\FX`.

**Manual install:** close REAPER and delete `reaper_animviewer.dll` from the `UserPlugins` folder and `rav_video_fx.clap` from `UserPlugins\FX`.

To find the `UserPlugins` folder: in REAPER, **Options > Show REAPER resource path in explorer/finder** (usually `%APPDATA%\REAPER\UserPlugins\`). Files named `reaper_animviewer.dll.old` or `.new` next to it are leftovers of an update and can be deleted too.

### Manual Installation

If you can't use ReaPack:

1. Download `reaper_animviewer.dll` and `rav_video_fx.clap` from the [latest release](https://github.com/DemuteTools/ReaAnimViewer/releases/latest).
2. **Close REAPER.**
3. In REAPER, **Options > Show REAPER resource path in explorer/finder** shows you the right folder. Copy the DLL into its `UserPlugins` subfolder (usually `%APPDATA%\REAPER\UserPlugins\`), and `rav_video_fx.clap` into `UserPlugins\FX\` (create the `FX` folder if it is missing). The `.clap` is only needed to render videos ([Rendering Video](#rendering-video)); always copy both files from the same release.
4. Start REAPER and look for **RAV: Viewer - Open viewer** in the action list.

A manual install does not update itself: repeat these steps for each new version.

### Troubleshooting

| Problem | Fix |
|---------|-----|
| **RAV: Viewer - Open viewer** does not appear in the action list | Restart REAPER after installing. Make sure you run the 64-bit Windows version of REAPER. With the Toolkit, make sure you clicked **Run** on the card once before restarting. |
| **Run** on the Toolkit card does nothing at all | Update the **ReaAnimViewer** card in the Toolkit (0.2.0 had this bug when a toolbar button for **RAV: Viewer - Open viewer** already existed), then click **Run** again. |
| **Run** says "ReaPack still lists ReaAnimViewer as installed" | The extension was once installed with ReaPack and its file was deleted by hand, so ReaPack still thinks it is installed. In the ReaPack window that opens, right-click the **ReaAnimViewer** package named in the message, choose **Uninstall**, click **Apply**, then click **Run** again. |
| **Run** says ReaAnimViewer is installed, but after a restart **RAV: Viewer - Open viewer** is still missing | Same cause as above with an older launcher: update the card first. If it persists, go to **Extensions > ReaPack > Browse packages...**, uninstall every installed **ReaAnimViewer** entry, click **Apply**, then click **Run** on the card and restart REAPER. |
| The viewer opens but stays black / shows an error | Update your graphics driver: the viewer needs OpenGL 3.3. |
| ReaPack reports a conflict on `reaper_animviewer.dll`, or the extension stays on 0.1.x | See [Upgrading from 0.1.x](#upgrading-from-01x). |
| ReaPack says the file is in use when updating | Close REAPER, reopen it, and run **Synchronize packages** again before loading a project. |
| The viewer shows **Can't load ...** (red) | The file can't be opened. The message says why and what to change in the export (for example a missing mesh, or an FBX saved as text). |
| The viewer shows **... has problems** (orange) | The file opened, but something is missing: textures, the animation, or the skin. The message says what to fix. It pops up once per file. |
| The message is gone and you want to read it again | While the current item has a problem, a red or orange **warning icon** stays in the bottom-left corner of the viewer. Hover it to see the message again. |
| You need to report a problem to us | Click **Copy details** next to the message (or **Copy error log** in the viewer menu) and paste the text into your bug report. |

---

## Getting Started

1. Open the viewer: **Actions > Show action list**, search **RAV**, run **RAV: Viewer - Open viewer**. Dock it wherever you like, REAPER remembers the position.
2. Drag a `.glb`, `.gltf` or `.fbx` file from Windows Explorer onto a track. An item is created with the exact length of the animation.
3. Move the playhead over the item: the character appears in the viewer, posed at that frame.
4. Press **Play**. The animation runs in sync with the transport: scrub, loop and stop as you would with a video.
5. Right-click and drag in the viewer to turn around the character. Start placing your sounds.

**Tip:** bind **RAV: Viewer - Open viewer** to a key or toolbar button to toggle the viewer quickly.

**Full screen:** the corners icon in the top-right corner of the viewer (left of the keyboard icon), **F11** while the viewer has the focus, or the action **RAV: Viewer - Full screen (toggle)** puts the viewer over the whole screen it is on. **Esc** (when nothing else in the viewer uses it: an open menu, a text field...), **F11** or the icon again put it back where it was docked. REAPER's keys without Alt keep working while it is full screen (Space plays).

---

## Supported Files

| Format | Extension | Notes |
|--------|-----------|-------|
| **glTF binary** | `.glb` | **Recommended.** Mesh, skeleton, animation and textures in a single file. |
| **glTF** | `.gltf` | Works; textures and `.bin` must stay next to the `.gltf` file. |
| **FBX** | `.fbx` | Works; textures are often referenced externally and can go missing. |

Each file should contain **one skinned mesh and its animation**. Animations built at runtime in the engine (state machines, blend trees, runtime IK) must be baked into the exported file. VFX, particles and engine shaders are not rendered.

---

## Camera Controls

| Input | Action |
|-------|--------|
| **Right-click + drag** | Orbit around the character |
| **Middle-click + drag** | Pan |
| **Mouse wheel** | Zoom in / out |
| **Navigation cube** (corner of the viewer) | Click a face, edge or corner to snap to that view |
| **Recenter camera** (viewer menu, **RAV view** tab) | Frame the character again |

---

## Model / Skeleton

The **Model | Skeleton** switch at the bottom left of the viewer (RAV view and Tagging view) shows the character's bones.

- **Skeleton** fades the character and draws its bones and joints on top.
- **Hover** a joint to see its name and the [roles](#roles) it plays. **Click** it to select it, click again to deselect. The bottom right of the view names the selected bone.
- In the Tagging view, the bones the selected rule reads take the rule's colour, and **+** next to a condition's bone menu uses the selected bone (see [Tagging your own events](#tagging-your-own-events)).
- A file with no animated skeleton (a static mesh) shows **No animated skeleton in this file** instead.

The switch and the selection are not saved: the viewer opens in **Model**.

---

## Viewer Menu

Click the **menu icon** in the top-left corner of the viewer to show or hide the options menu. It opens as a panel in that corner, in front of everything else in the viewer, and stays open while you tune: the render changes as you go.

On its left, a column of tabs picks which settings show:

| Tab | Settings |
|-----|----------|
| **Global** | Shared by the three views: [Light](#light), [Ground](#ground), [Shadow](#shadow), [Performance](#performance) |
| **RAV view** | **Recenter camera** (frame the character again) and **Model / Skeleton** (the same switch as the bottom left, see [Model / Skeleton](#model--skeleton)) |
| **Tagging view** | **Markers written**: which markers Commit writes, Take, Project or Both (see [Markers](#markers)) |
| **Video view** | **Video panel** (show or hide it), **Ask before deleting a shot**, **REAPER catch-up** (see [Rendering Video](#rendering-video)) |

The tab follows the view when you change view (toggle or **V**); otherwise the tab you picked stays. On a short viewer the settings scroll; the tabs stay in place.

Under the tabs:

- **Copy error log**: Copy the recent load errors, the viewer version and your graphics card info to the clipboard, ready to paste into a bug report.
- The installed version.

### Light

- **Colour**: Colour of the main light.
- **Position**: Drag in the pad to move the light around the character.
- **Ambient**: Fill light. Lower it for more contrast.
- **Specular**: Strength of the shine on surfaces.
- **Relief**: Strength of the normal maps (surface detail).

### Ground

- **Enable**: Show or hide the floor and grid under the character.
- **Grid: 1 m / 10 m**: Size of one grid cell in real-world metres, whatever the file format (FBX centimetres and glTF metres are converted for you). A 100 m dragon covers 10 cells at 10 m.

### Shadow

- **Off / Low / Mid / High**: Quality of the shadow cast on the floor. Off is the lightest for your GPU.

### Performance

| Option | Description |
|--------|-------------|
| **Normal maps** | Turn surface-detail maps on or off |
| **MSAA** | Edge smoothing: Off, 2x, 4x, 8x |
| **FPS** | Show the frame rate in the viewer |

**REAPER catch-up** (in the **Video view** tab) shows how long REAPER's Video window takes to catch up with an output size or display change while playing (see [REAPER catch-up while playing](#reaper-catch-up-while-playing)).

If the viewer feels slow on a laptop, lower **MSAA** and **Shadow** first.

---

## Shortcuts

Click the **keyboard icon** in the top-right corner of the viewer (left of the FPS readout) to open the **Shortcuts** window, a large panel over the viewer. **Esc**, its **✕** or a click outside it closes it. The keys work while the viewer has the focus; every other key stays REAPER's.

The window has three parts:

- **Sections** on the left: All, then Viewer, Video, Tagging, Sound Edit (the Library's Edit window), Timelines (scroll and zoom of every RAV timeline) and Mouse. The search field at the top filters the rows by name.
- **The table**: one line per shortcut. *Action*, *Where* it works, the *Keys* that run it now, what it *Follows* (REAPER or Own), its *Source* (the REAPER action it follows, or "RAV only") and the chain button.
- **The detail** on the right, for the selected row: its keys, what it follows, its default, and anything in its way (a key another row or window takes). Its buttons: **Record key…**, **Link…** and **Default**.

Double-click a row to record a new key, or for a Timelines row a key or a turn of the wheel (Ctrl, Shift and Alt can be held with it; Esc cancels). Right-click a row for its menu: **Record key…**, **Link to REAPER action…**, **Back to default**. Mouse gestures are listed for reference (greyed, fixed).

### REAPER or Own

Every row is either:

- **REAPER**: it runs on the keys you give its action in REAPER's Actions list, read live. The viewer rows have their own RAV actions ("RAV: Viewer - …", "RAV: Video - …", "RAV: Tagging - …"); REAPER runs them too outside the viewer. The Sound Edit rows follow REAPER's own item actions (split, trims, heal, solo / mute, next / previous item, repeat, set snap offset to cursor, go to previous marker / project start, set take channel mode to normal / mono downmix / left / right for the Sound's Mono, set fade-in / fade-out shape to linear, slight / sharp convex, slight / sharp concave, slight / sharp S-curve on the selected zones): your REAPER keys for them do the same thing in the Sound Edit window while it has the focus. **Exclusive solo selected part** has no REAPER action to follow (REAPER does it with Ctrl+Alt+click on a Solo button): it is a RAV action with no key until you give it one; pressed on the only soloed part it unsolos it. **Left / Right: set Left / set Right / follow rule** (the selected variations' side) are RAV actions with no key until you give them one. Split, Heal and Loop use **B**, **H** and **R** while REAPER gives their action no key. The Timelines rows follow your REAPER bindings of its view actions (scroll and zoom, wheel and keys).
- **Own**: RAV's own key (or wheel, for the Timelines rows), or a REAPER action you link the row to.

The footer says when `reaper-kb.ini` was last read; RAV reads it again whenever REAPER saves it.

### Linking a row to any REAPER action

Click a row's chain (in Own) to link it to any action of REAPER's Main section: RAV then runs the row on that action's keys (and wheels, for a Timelines row), followed live. Find the action by:

- its **name** (words, any order: "split edit"),
- its **command ID** ("40757", "_SWS_ABOUT"; REAPER's Actions list: right-click > Copy selected action command ID),
- a **shortcut** it has, typed ("Ctrl+S", "Shift+Wheel") or pressed: click the keyboard button next to the field, then press the keys or turn the wheel.

Each action in the list shows its keys. Click one to check it (its name, and the keys RAV will follow), double-click or **Link** to link it. **Unlink** (or **Default**) gives the row back its own key. A key another RAV row holds stays that row's.

### Find by key

**Find by key** (top bar): click it, then press a key combination or turn the wheel with its modifiers. The window lists every action of REAPER's Main section bound to it (REAPER's defaults included) and every RAV row that runs on it, plus the keys the Sound Edit window keeps for itself (Space, Shift+Space, Esc, E) and, for the plain wheel, the 3D view's fixed Zoom. **Link to…** on a REAPER result links a row to that action at once (the row becomes Own, linked); the footer's **Undo** puts the row back. **Show** selects a RAV row. Esc clears the result.

### Default keys

| Key | Action | Where |
|-----|--------|-------|
| **V** | Next view: RAV / Tagging / Video | Anywhere |
| **P** | Show / hide the Video panel (from RAV view or Tagging view it also switches to Video view) | Anywhere |
| **F11** | Full screen on / off (**Esc** also leaves full screen) | Anywhere |
| **L** | Clip list: open / close | Anywhere |
| **C** | Cut at playhead | Video view |
| **Delete** | Delete current shot | Video view |
| **Ctrl+C** | Copy shot camera | Video view |
| **Ctrl+V** | Paste shot camera | Video view |
| **E** | Add event at playhead | Tagging view |
| **Delete** | Suppress / delete event | Tagging view |
| your REAPER keys | Trims, split (**B**), heal (**H**), solo / mute the selected part, next / previous variation, Loop (**R**), snap offset at the cursor, previous zone start / file start, Mono off / sum / left / right, fade-in / fade-out shapes (also the **Fade in** / **Fade out** buttons above the waveform) | Sound Edit |
| **Ctrl+Shift+wheel up** | Gain of the selected zones: +1 dB per notch (notches in a row = one undo point) | Sound Edit waveform |
| **Ctrl+Shift+wheel down** | Gain of the selected zones: -1 dB per notch | Sound Edit waveform |
| **Ctrl+Alt+drag** along a fade | Its curve, as REAPER: up = convex, down = concave (the small diamond in the fade's middle does the same; double-click it: its shape's own curve). With several zones selected, all their fades on that side follow | Sound Edit waveform |
| your REAPER bindings | Scroll and zoom (wheel, Alt+wheel, Ctrl+wheel, +, -, Up, Down by default) | Timelines |

A key you bind is taken from REAPER while the viewer has the focus: bind Space, for example, and Space no longer starts or stops playback from the viewer. Hold Ctrl, Shift or Alt with it to keep REAPER's own key free. The key caps and tooltips in the viewer show your keys, custom ones in the accent colour. On AZERTY and other layouts with an AltGr key, Windows treats Ctrl+Alt+key as AltGr+key, so a Ctrl+Alt binding may type a character instead.

Your keys are kept on this computer, for every project.

---

## Working with Animation Items

Animation items behave like regular REAPER media items:

- **Move** them to realign the animation with your sounds.
- **Trim** the start or end to show only part of the animation.
- **Loop** an item by extending it past its end.
- Put animations on **several tracks**: when items overlap under the playhead, the viewer shows the one on the **topmost track**.

Animation items produce no audio: you can place them on the same tracks as your sounds or on a dedicated "Anim" track.

---

## Rendering Video

ReaAnimViewer can put the animation in the videos you render from REAPER, with the session's audio, through REAPER's usual Render dialog and Region Render Matrix. Nothing is screen-recorded and RAV writes no video file: a small video FX, **RAV Video FX**, installed with the extension, hands REAPER the picture of its track's animation for every frame REAPER draws, in playback and in renders.

### Adding the video FX

1. Open the viewer and click **Video view** at the top (or press **V** until it shows). The **Video panel** opens on the right. **P**, the panel button at the top right, or **Video panel** in the viewer menu's **Video view** tab show or hide it.
2. The panel follows the track of the animation shown in the viewer. When it says **No video FX on this track**, click **Add video FX to track**. The action **RAV: Video FX - Add to selected track** does the same from REAPER.
3. That's it. You never need the FX window: RAV drives the FX for you.

One FX shows the animation items of its own track. Where that track has no item, the FX shows nothing, so the video tracks below show through. Add the FX to every animation track you want in the video. REAPER's Video window (**View > Video**) shows the result too.

### Video view

- **RAV view** is your free camera. **Video view** is the render's camera. Switching between them never changes the free camera.
- In Video view the frame shows exactly what the video will show, at the output's shape. The size and frame rate sit above it on the left, the current shot's name on the right. Nothing is drawn inside the frame.
- Right-drag (orbit), middle-drag (pan), the wheel (zoom) and the navigation cube edit the shot under the playhead. Each gesture is one undo step.
- The panel's inspector edits the shot under the playhead: its name, **Cut to next** / **Move to next**, Yaw, Pitch, Distance, Target X/Y/Z, **Copy RAV view** (the shot takes your free camera) and **Frame model** (the whole model, same angle).
- **Track**: the panel follows the track of the item shown in the viewer, or you can pin one track.

### Shots

The video camera is a sequence of **shots**. A shot starts at a time and keeps its camera until the next shot (**Cut to next**), or moves smoothly to the next shot's camera (**Move to next**).

- **Shot strip** (under the viewport, in Video view): the shots over the current item, and REAPER's playhead as a white line. A ruler above it shows the item's time in seconds (`0:02`) and, zoomed in, frames (`+12f`). Click or drag in it to move the playhead; double-click a shot to put the playhead on its first frame. Drag a line between two shots to retime that cut: it snaps to frames, each shot keeps at least one frame, the first shot's start stays put, and it is one undo step. Any camera move into or out of that cut speeds up or slows down to fit. Turn on **Snap** (next to Cut) to land drags and scrubs on the ruler's ticks; it is off by default and resets when REAPER restarts.
- **C** (in Video view), the strip's **Cut** button or **+ Cut at playhead** in the panel: a new shot starts on the frame under the playhead, with the camera shown there. Then reframe it with the mouse. A cut inside a **Move to next** shot makes that move end at the cut: it now eases into the camera shown at the cut instead of continuing to the next shot.
- **Shot list** (panel): click a shot to move the playhead to it; **x** deletes it (the first shot stays).
- **Saved angles** (panel): **+ Save** keeps the camera of the shot under the playhead under a name. Click a saved angle to give it to the shot under the playhead; right-click it to delete it. Saved angles are stored in the FX, with the project.

Shots are ordinary automation: envelope points on the FX's six parameters (Yaw, Pitch, Distance, Target X/Y/Z), square points for a cut, smooth ones for a move. You can edit them in REAPER's envelope lanes, and anything that drives FX parameters (envelopes, modulation, DM-XYZ-Pad) drives the camera. Distance and target are relative to the model's size, so a shot keeps working when the animator re-exports at another scale.

### Output

- **Size**: the project's video size (**File > Project settings > Video**) by default, or an override for this FX: vertical, square, 4K or a custom size.
- **Frame rate**: always the project's.
- **Background**: the viewer's background, or **Transparent** (where there is no model, the picture is transparent, so the video tracks below can show through when REAPER composites the FX's transparency).
- Light, floor, grid, shadow and MSAA follow the viewer's **View** menu (with the viewer closed, its last settings). The menu, navigation cube, FPS and shot strip never appear in the video.

### Rendering

1. In the Video panel, click **Matrix** (top of the panel) to open REAPER's Region Render Matrix and tick the regions to render, or **Render** to open REAPER's Render to File dialog.
2. In the Render dialog, choose the **Source** (Region render matrix, Time selection, Entire project...) and a video **Format** (for example MP4 / H.264). Keep the video size on the project's settings, or set the size shown above the Video view frame.
3. Render. The picture comes from the FX at each frame's exact time, so picture and sound stay in sync and each cut lands on its frame.

RAV never changes your render settings.

### REAPER catch-up while playing

While playing, REAPER prepares video frames a few seconds ahead (its read-ahead). Camera moves and playback stay in sync in REAPER's Video window. An output size or display change (light, floor, background...) waits behind that read-ahead: it can take up to that long to reach REAPER's Video window while playing. Video view in RAV is always up to date, and rendering has no offset: every frame is drawn for its own time.

While REAPER's Video window is open, the line above the Video view frame shows REAPER's current read-ahead, for example `REAPER catch-up 1.5 s` while playing, or `REAPER catch-up: live` when stopped (hover it for details). It is how long such a change can take to show there, not a measurement of a particular change. On a narrow view it shortens to `catch-up 1.5 s`, shortening the shot name if needed. Turn it off with **REAPER catch-up** in the viewer menu's **Video view** tab.

### If the video FX shows nothing

| What you see | What to do |
|--------------|------------|
| The panel says **No video FX on this track** | Click **Add video FX to track**. Check that the panel follows the right track (**Track** in the panel). |
| **Video FX bypassed** or **Video FX offline** | Click **Enable FX** or **Set FX online** in the panel. |
| **Video FX inactive** | The FX and the extension come from different releases. Update ReaAnimViewer (ReaPack: **Synchronize packages**; Toolkit: **Update**, then **Run** on the card) and restart REAPER. |
| A message says the RAV video FX is not installed | `rav_video_fx.clap` is missing from `UserPlugins\FX`. Update or reinstall ReaAnimViewer (see [Installation](#installation)) and restart REAPER. If it is there, rescan plug-ins in **Options > Preferences > Plug-ins > CLAP**. |
| No picture over an item | The FX shows only items of its own track. Put the playhead over a RAV item on the FX track, and check that the viewer can load that file (no red message). |
| REAPER's Video window stays black | Open it with **View > Video**. A video item on a track above can cover the FX's picture. To check that the FX works at all, turn on the action **RAV: Video FX - Test pattern**: an orange frame with a moving bar means the FX and the extension talk to each other. Turn it off again. |
| The rendered video has the wrong size | Check **Output > Size** in the panel, and the video size in the render format's settings. |

To report a problem, click **Copy error log** in the viewer menu and paste it into your report.

---

## Auto-Tagging (Event Markers)

ReaAnimViewer can find events in an animation from the way its bones move, and mark them on your timeline: footsteps, a hand hitting something, a sword swing, a body falling, a head turn, a clap... Feet, and hands that grab or let go, are found for you by **Smart Detection**. For anything else you can describe as "this bone goes above or below this value", you write **rules** (or load a preset). You check the result in the **Tagging view**, and commit. Each event becomes a marker named after its rule, on the item (take marker), on the timeline (project marker), or both. Then place your sounds on the markers instead of hunting for each frame by eye.

Footsteps are the ready-made starting point: under **Smart Detection**, tick **Step** and press **Detect** (or load the **Footsteps** preset, which does the same). Lift-offs, slides and pivots of the feet, and the hands' grabs, releases and pivots, work the same way. For everything else, you build your own rules, then save them as presets to reuse them.

Each animation item keeps its rules, thresholds and corrections, saved in the project. Items that play the same animation file share them, like REAPER's pooled MIDI items (see [Copies of one animation share their tagging](#copies-of-one-animation-share-their-tagging)). Nothing is written to the animation file.

### Quick start: footsteps

1. Open the viewer, then click **Tagging view** at the top (or press **V** until it shows). A strip appears under the character and a panel on the right.
2. Put the playhead over an animation item. In the panel's **Smart Detection** tab, click the **Step** chip to tick it, then press **Detect** (at the bottom of the panel).
3. The strip shows one row per foot (`FS L`, `FS R`) with a tick for each step found. Play or scrub to check them against the character.
4. Too many or too few? Drag the step's **Sensitivity** (higher finds more). Always a little early or late for your sounds? Drag its **Offset**. The markers follow as you drag. Then correct single steps if needed (see [Correcting events](#correcting-events)).
5. As soon as you change something, RAV shows the result on the timeline as **preview markers** (`FS L - Preview`, in a darker colour). Nothing is final yet.
6. Select the items to tag in REAPER (the panel footer says how many), then click **Commit to N items**. The previews are replaced by the real markers. **Cancel** instead puts the items back as they were at their last Commit.

To tag several walk cycles at once, set them up one by one (steps 2 to 4), select them all and click **Commit** once: each item is tagged with its own rules. Copies of one cycle (the same file duplicated to build a longer walk) are set up once: they share their tagging.

### Smart Detection

Smart Detection reads the body as physics: it measures the legs to know the character's size, finds where each part of the foot, and each hand, comes to rest (a floor, a stair, a ledge, a hold, a table), and places the markers from that. One setting works on every clip: there are no thresholds to tune, and no **Analyse**.

- Every event type is a **chip**, grouped by the body part that makes the sound: **Legs**, **Arms** and **Body** (the trunk and the head). The common ones (Step, Lift-off, Jump, Land, Grab, Release) come first, in bold. **Click a chip's name** to tick or untick the type; **click its ⚙** to see its settings in the strip under the list (an unticked type is ticked at the same time; its ⚙ again hides them). A category's caption shows how many of its types are ticked (`2 / 11`); its **×** unticks them all. The chip list takes the tab's free height and is the only part that scrolls. The settings strip keeps one compact height, so nothing moves while you tick, search or switch types: its header (the type's tick, its category, **(i)** with its whole description, **×** to hide it), then the type's description in the lines its settings leave free (an amber note, such as "Raised by Detect on FS R", comes first), **Sensitivity**, **Offset** and **Per side**, the type's own options, and its **Sound** at the bottom. A type that needs more room (Per side open on a type with many rows) scrolls inside the strip. An amber dot on a ⚙: Detect raised that type's sensitivity, or found nothing up to 100 %; the tab shows an amber dot too while the **Rules** tab is open.
- **Is looping** (beside the search, saved on the item, one undo step): the animation loops, its end continues into its start, so a contact, step, grab or release that crosses the loop point is found whole.
- **Search** fades the chips that do not match (by name, marker, category or description) without moving them.
- **Presets** are your own sets of ticks, kept on your PC for every project. **+ Preset** saves the types ticked now under a name; clicking a preset ticks exactly its types and unticks the others (press **Detect** to apply them, one undo step); a preset shows a tick when the ticks are exactly its own. Its **⚙**: **Rename...**, **Store current ticks** (it takes the ticks of now), **Delete...**.
- **Tick** the event types you want, then press **Detect** (one button for every category):
  - **Legs**
    - **Step**: where the foot lands. **Combined** gives one marker per landing (`FS L`, `FS R`), at the first part of the foot to touch. **Separate** gives the heel and the toe their own markers (`Heel L`, `Toe L`, `Heel R`, `Toe R`). The toe is the ball of the foot (the toe bone), or the toe tip on a rig without a toe bone. A foot that glides into place (high heels on a catwalk) gets its step where it stops.
    - **Lift-off**: where the foot leaves the ground (`Lift L`, `Lift R`). **Combined** gives one marker per lift-off, where the heel and the ball have both left the ground (the toe tip, which rolls off a little later, counts only on a rig without a ball bone). **Separate** puts each one on the block of that part: `Lift Heel L` for the heel, `Lift Toe L` for the ball or the toe tip.
    - **Slide scuff**: where a planted foot slides on the ground (`Slide L`, `Slide R`).
    - **Pivot scuff**: where a planted foot turns on its ball or its heel (`Pivot L`, `Pivot R`): flat, on tiptoe with the heel up, or on its heel with the toes up. A foot that turns as it rolls onto its toes and stays there (a swivel) pivots; so does a wide turn that ends as the foot leaves the ground (a spin turn, of 30° or more by default; a higher pivot sensitivity lowers that angle); a small twist as the foot leaves (a heel whip at toe-off) does not.
    - **Start** / **Peak** on both scuffs (and the hands' **Pivot scuff**): the marker where the scuff starts (**Start**, the default) or where the part on the floor rubs fastest (**Peak**: where a dance's swivel squeaks).
    - **Jump**: where a jump takes off, on the last foot to leave the ground (`Jump L`, `Jump R`: one marker per jump; when both feet leave together, within 10 ms, it is `Jump L`). A jump is found in three ways: by its flight, both feet off the ground long enough (300 ms by default) while the body falls freely; by a hard push off the ground, the hips accelerating up well above body weight while a foot is down, with both feet leaving right after (even briefly); or by a free fall, both feet off as long as a flight and the body falling freely over its first 300 ms, however it ends (a jump that ends on the back or the side gets its Jump, and no Land). Feet off the ground while the hands hold the body up (hanging, climbing) give none. A run's strides push as hard as a jump, so a push counts only while the body is not running (about 2 m/s); a jump taken while running is found by its flight. A run's own flights (both feet off at every stride, longer as it speeds up into a sprint) are never jumps, whatever the sensitivity: a flight counts as a jump only when it stands out from the flights just before and after it (2.5 times longer), so a jump taken mid-run is still found. A leap from one foot onto the other, like the *Jumping* sample onto a ledge, gives its Jump by its push. A clip that starts during the push (the take-off foot already on the ground at the first frame) gets its Jump too, unless **Is looping** is on. A higher sensitivity counts shorter flights and softer pushes. **Separate**: on the part that leaves last, of the heel and the ball as for **Lift-off** (`Jump Heel L`, or `Jump Toe L` for the ball, or the toe tip on a rig without a ball bone). With **Lift-off** ticked too, `Jump` replaces the lift-offs within 30 ms of it (a `Jump Heel` / `Jump Toe` only the lift-offs of its own part, when lift-offs are Separate too).
    - **Land**: where the jump lands, on the first foot to touch, at that foot's step (`Land L`, `Land R`; both feet together, within 40 ms: `Land L`). When the second foot touches 40 to 90 ms after the first, the two impacts are heard apart: each foot gets its own Land (`Land L`, then `Land R` at its own step); later than that, the second foot's touch is a step. A take-off keeps one Jump: the foot that leaves first is a lift-off. **Separate**: on the part that touches first (`Land Heel L`, `Land Toe L`). A foot that lands sliding fast along the floor holds no contact, so it gives no Land. With **Step** ticked too, `Land` replaces the steps (`FS`, `Heel`, `Toe`, either foot) within 30 ms of it; a `Land Heel` / `Land Toe` replaces only the `Heel` / `Toe` steps (or `FS`), so the heel slap that follows a toe landing keeps its own marker.
    - **Tap**: where the toes touch the ground lightly while the other foot carries the body (`Tap L`, `Tap R`): a touch of the ball or the toe tip under 150 ms, the heel up, the other foot on the same floor. Steps of a walk or a run are never taps. A higher sensitivity counts longer touches (up to 300 ms). With **Step** or **Lift-off** ticked too, `Tap` replaces that foot's step at the touch (`FS` or `Toe`) and its lift at the end of the touch: a tap is one sound. Detect never raises Tap or Drag on a clip that has none.
    - **Drag**: where the toes slide along the ground while the foot moves, the heel up (`Drag L`, `Drag R`): a foot dragged behind, the feet of a knee crawl, the back foot of a kneel. The marker sits where the slide starts. A walk's or a run's toes skimming the floor as they leave or land are no drag; a foot moving fast (about 2 m/s) is swinging, not dragging. A higher sensitivity counts shorter drags (100 ms by default).
    - **Heel Tap**: where the heel drops back onto the floor while the ball of the foot stays down (`Heel Tap L`, `Heel Tap R`): the foot rolls onto its ball, the heel rising a centimetre or more, then comes down, as in a dance. The marker sits where the heel reaches the floor. A foot lifted flat, a foot on a climbing hold or a crawl's toes give none; a heel kicked up and slammed down fast enough to leave the ground is a **Step** (`Heel`), not a heel tap. A higher sensitivity counts smaller heel rises. With **Step** ticked too, `Heel Tap` replaces that foot's step at the same moment. Detect never raises it on a clip that has none (a weight shift lifts the heel a little).
    - **Knee**: where a knee touches the ground, kneeling or crawling (`Knee L`, `Knee R`): the knee comes down and rests close to the floor, or comes down onto the floor and slides on (a low crawl dragging its knees). A knee that stops a little above the floor (a deep crouch), or legs that stop sliding along the floor, give none.
    - **Kick**: where a foot whips through the air on a raised, stretched leg (`Kick L`, `Kick R`): its speed against the body's, at its fastest (**Peak**) or where the motion starts (**Start**). A running stride's foot moves as fast, but it is folded under the hips when it is up and low when the leg is stretched, so runs give no kick. **Separate**: on the part fastest at the peak (`Kick Heel L`, `Kick Toe L`).
  - **Arms**
    - **Grab**: where the hand lands on something and stays: a hold, a ledge, a table, the floor (`Grab L`, `Grab R`). A hand that only slows down, or stops for an instant between two moves, is no grab.
    - **Release**: where the hand leaves what it held (`Release L`, `Release R`).
    - **Pivot scuff**: where a planted hand turns on what it rests on, a hand on the floor or a ledge while the body swings round it (`Hand Pivot L`, `Hand Pivot R`). The turn is the hand's own twist: a vault where the palm stays put gives no marker. A hand that lands turning, or leaves at once, does not pivot. **Only after a Grab** (on by default): only a hand that came down with a grab (or rests from the start of the clip) can pivot. Off: any resting hand can, a hand placed gently too, but a hand that twists while held still in the air can then give an extra marker.
    - **Elbow**: where an elbow touches the ground, lying or crawling on the elbows (`Elbow L`, `Elbow R`): the elbow comes down and rests on the floor.
    - **Clap**: where the two hands meet at speed and stop against each other (`Clap L`, `Clap R`: one marker per clap, on the hand that moves faster). Hands that pass each other, or come together slowly, give none. A higher sensitivity counts slower claps, and hands meeting a little further apart; Detect never raises it by itself, so a clip without a clap stays without one. Two hands joining on a two-handed grip (a pistol, a rifle) meet as a clap does and give one.
    - **Hand on body**: where a hand lands at speed on the body, the trunk, the head or a thigh, and stops there (`Hand on body L`, `Hand on body R`). A hand that swings past the body, or comes to rest slowly, gives none, nor does the body coming onto a hand held still (a climb's last pull); a hand grabbing gear worn on the body (a holster) is no touch. A higher sensitivity counts slower touches, a little further from the bones; Detect never raises it by itself, so a clip without a touch stays without one.
    - **Slap**: where a flat hand hits something, the floor, a wall or the body, and the fingers stay straight (`Slap L`, `Slap R`): a hand crawling on the floor, landing on its palms, slapping a thigh. A hand whose fingers close after the contact (a hold, a rung, a pistol grip) is a **Grab**, and a fist is never a slap. With **Grab** or **Hand on body** ticked, a slap replaces that hand's grab or hand on body: one marker per contact. It reads the fingers, so it needs finger bones that move in the animation: on a rig without fingers, or whose fingers do not move around the contact (within half a second of it), it finds nothing and the contacts stay grabs. A contact in the last 0.15 s of an animation that does not loop stays a grab (the fingers can't be seen after it). Detect never raises it by itself.
    - **Swing**: where a hand whips through the air (`Swing L`, `Swing R`): its speed against the body's, at its fastest (**Peak**) or where the motion starts (**Start**). A hand resting on something gives none. The default catches strikes and big throws of the arm; the arms of a walk or a run stay below it, but swinging the arms into a jump can reach it. A swing, kick or punch already under way on the first frame (begun before the animation) gives none, unless **Is looping** is on. A straight thrust away from the shoulder is a **Punch**, not a swing.
    - **Punch**: where a hand thrusts straight out from the shoulder (`Punch L`, `Punch R`), at its fastest or at its start, as **Swing**: the arm opens from bent to straight, fast, at about shoulder height or a little lower. A hook or an uppercut (an arc, the arm already open) is a **Swing** when fast enough; an arm thrown straight down (to the floor, for balance) or reaching up (a climb's reach for the next hold) is no punch. A higher sensitivity counts slower punches.
    - **Jump** and **Land** (Arms): the arms of a jump. **Jump**: where an arm is thrown up into the take-off (`Arms Jump L`, `Arms Jump R`); **Land**: where an arm comes down at the landing, reaching for the floor or the knees (`Arms Land L`, `Arms Land R`); both at the hand's fastest. They follow the take-offs and landings that **Legs > Jump / Land** find at their default settings (ticked or not; changing their sensitivity does not move the arms), so an animation with no jump has none; a landing on both feet apart (two Lands) still gives one Arms Land per arm. With **Swing** ticked, they take that arm movement from it: the swing gives way, one motion, one sound, and Swing keeps the swings that have nothing to do with a jump. A hand that hits the floor is still a **Grab**. A higher sensitivity counts slower arms.
  - **Body**
    - **Jolt**: where an impact jolts the whole body, for gear and cloth layers (one `Jolt` marker, no side): a step, a jump's landing, a knee coming down, the body sitting down or a fall landing on the ground (as **Knee**, **Sit** and **Fall** find them, ticked or not), where the body (the hips) is thrown up hard, 1 g or more by default, which means the ground pushes with about twice the body's weight. Runs, sprints and landings give one; ordinary walking steps do not. Raise the sensitivity to catch softer steps (0.5 g at 100 %), lower it to keep only the hardest landings (2 g at 0 %). A landing and its steps make one jolt. A clip with no jolt (a walk, a climb) stays without one: **Detect** does not raise this sensitivity. The inspector's **Strength** is the peak, in g.
    - **Spin**: where the whole body turns round, a turn on the spot, a dance spin, a turn while walking (`Spin` where the turn starts, `Spin End` where it stops; no side). It is read from the hips: a turn starts when they turn faster than 15° per second, ends when they slow under 5° per second or turn back, and counts when it turns 45° or more, so a walk's hip sway gives none. A turn already under way when the clip starts, or still going when it ends, gets its marker at the clip's edge. A higher sensitivity counts smaller turns (22° at 100 %); Detect never raises it by itself, so a clip without a turn stays without a Spin. The pelvis twisting while the hands hold on (climbing) can give a Spin of about 50°. The hips turning out and straight back (a punch's twist) give none: two turns back to back, opposite ways and of similar size, are a twist.
    - **Sit**: where the body sits down, on a seat or on the floor (`Sit`, one marker for the body): the hips come to rest low, the back upright and the hips away from the feet. A squat (the hips over the feet), a kneel (a knee down) or lying down gives none.
    - **Fall**: where the body lands on the ground, on the hips or the back, after a fall (`Fall`): the hips or the back come close to the floor after falling fast, such as a fall on the back after a free fall. Lying down slowly gives none, nor does a squat, however deep and fast (the back upright over the feet); a higher sensitivity counts slower falls.
- **Detect** lights up while a change waits for it: a type ticked or unticked, a type switched between **Combined** and **Separate**, a whoosh (Kick, Swing, Punch) or a scuff switched between **Peak** and **Start**, or the hands' **Only after a Grab** switched. It writes them in one undo step. Your corrections follow a switch: from **Combined** they go to the heel's row, from **Separate** back to the combined row of that foot. Each ticked type adds one row per foot (or per hand; one row for **Jolt**, two for **Spin**) to the strip, next to the rules' rows.
- **Sensitivity** (%, one per type): higher finds more events, lower fewer; 50 % is the default. **Offset** (ms, one per type) moves every marker of that type, for example +20 ms if your sound should land after the contact. On a type already detected, both apply at once: the markers follow while you drag, and releasing is one undo step.
- **Click an event** of a Smart Detection type on the strip: the **Smart Detection** tab opens and its settings strip shows the event instead (**Event** mode, same place): `< Jump > Event Jump L`, **Go to event**, **Suppress** (or **Delete** / **Restore**), its **State**, **Time**, **Strength** (the foot's or hand's speed, the turn's angle, the jolt's g), **Speed**, **Found by** and **Confidence**, then its own sounds; `< Jump` goes back to the type's settings, **×** closes it. An event of a rule opens the **Rules** tab instead.
- Marker names stay plain (`FS L`): the strength of an event shows in its Event view only. Choosing a sound by strength will come with a later version.
- **Doubtful events (amber):** each event Smart Detection finds gets a confidence: how much stricter its thresholds can get before it disappears (100 %: still found with thresholds twice as strict; 0 %: only just over one; for **Punch**, 1.4 times as strict, a jab's speed sitting close over its threshold). Below the limit set in the viewer menu, **Tagging** > **Doubtful events** > **Amber below** (40 % by default; 0 % turns it off), the event shows in amber with a `?` in the strip, its inspector says why, and its marker is written in amber (its name unchanged): check those by ear first. The limit is one setting for every type, kept across sessions; changing it shows previews until the next **Commit**. The confidence only knows the thresholds the sensitivity moves: an event that clearly passes them but is not the motion the clip means (a deep crouch whose knee stays just off the floor, two hands joining on a pistol grip read as a clap) is not amber.
- Kick, Swing, Punch and the arms' Jump and Land are never raised by **Detect** when an item has none: most animations have no whoosh, and raising them would read a run's arms as swings. Raise their sensitivity yourself to catch slower motions.
- Correct single events as on any row (see [Correcting events](#correcting-events)): your corrections survive a new **Detect** and a sensitivity change. After an offset change, check them: a suppressed event stays masked only while the detection is within 30 ms of where it was, and your own events stay where you put them. Unticking a type removes its rows and the corrections on them.
- Click a row's name in the strip to select it: the **Smart Detection** tab shows its type's settings. Its marker name and colour are in the **Rules** tab's inspector (switch to that tab after clicking the row), where you can change them.
- **Roles:** Smart Detection needs, for at least one leg, the heel, the knee and the hip (up leg; the hips will do), or else the knee and the hip (up leg) alone; per foot the heel and the toe (or the toe end); per hand its hand bone; for **Jump**, **Land** and **Jolt**, the hips bone plus a heel, toe or toe end on both feet; for **Spin**, both hips (up legs); for **Tap**, **Drag** and **Heel Tap**, the heel and the toe (or the toe end) of the foot; for **Knee** and **Elbow**, that knee or elbow; for **Sit**, the hips and the chest (or spine); for **Fall**, the hips or the chest (or spine); for **Clap**, both hands; for **Hand on body**, its hand and two body bones in a row (the hips and the spine, or the hips and an up leg...); for **Slap**, its hand, the knuckle (the base of a finger: the middle one, else the index, else the ring finger) and the fingertip of that finger. **Sit** and **Fall** also need a foot bone, to find the floor. Kick, Swing and Punch also need the chest (or the spine, or the hips); the arms' Jump and Land need that and what Jump and Land need. When a ticked type misses one, the panel names it in red (at the top of the chip list) and that foot or hand gets no events of that type: map the bone in **Roles**. A rig without foot bones still gets its hand events, as long as the knee and the hip (up leg) of one leg are mapped.
- Smart Detection and rules work together on one item: each has its tab (the **Rules** tab lists only the rules), and both make markers. A preset saves both.

### Tagging your own events

1. In the Tagging view, put the playhead over the item and click **+ Rule**. Give the rule the name you want on the markers (for example `Whoosh R`) and a colour.
2. Set its condition: pick the **bone** to watch, **what** to measure (position, speed, acceleration, stillness, relative drop), in **which direction** (vertical, horizontal, total...), and **from** what (the floor or another bone). The bone menu lists the roles first, then every bone of the skeleton, including a weapon or prop bone if it is part of the rig. Not sure which bone is which? Switch the 3D view to **Skeleton** (bottom left), click the bone on the character, then click **+** next to the condition's bone menu: it uses that bone, or its role when the bone plays one. When it plays several, it uses the role the bone is named for (`ball_l` is the toe, not the toe end), else the bone.
3. Watch the signal in the strip while you scrub, and drag the threshold line to where the event happens. Add more conditions with **+ AND condition** if one is not enough.
4. Choose where the marker lands: **at the start** (the moment the threshold is crossed) or **at the highest / lowest point** of a signal (for example the fastest moment of a swing).
5. Happy with it? **+ Save as...** in the preset menu, and load it on your other clips.

A few ideas to start from (thresholds depend on your character and animation, use **Analyse** or drag the lines):

| Event | Condition | Marker at |
|-------|-----------|-----------|
| Whoosh of a swing or punch | Bone: right hand (or the weapon bone), **Speed**, total, **above** a fast value | the highest point |
| Clap | Bone: right hand, **Position**, total, **from** the left hand, **below** a few cm | the start |
| Body fall | Bone: hips, **Position**, vertical, **from** the floor, **below** a low height | the start |
| Crouch, cloth rustle | **Joint angle** at a knee **below** a bent angle | the start |
| Head turn | **Rotation** of the head, **Speed**, **above** a fast value | the highest point |
| Hand on a table | Bone: hand, **Position**, vertical, **from** the floor, **below** the table's height, AND its **Speed** **below** a slow value | the start |

### Rules

A rule produces one kind of marker. It says **when** an event happens (its conditions), **where** the marker lands, and **how often** it may fire. Click a rule in the panel's **Rules** list to see it in the inspector below.

- **Rules list:** the switch turns a rule on or off, the number is how many markers it makes on this item, and the two icons **Duplicate** and **Delete** it. **+ Rule** adds an empty one.
- **Name and colour:** the name is the marker's name on the timeline; the colour dot sets the marker's colour (project markers) and the rule's colour in the strip.
- **When all of these hold:** the rule's conditions. They all have to be true at the same time. Each condition reads like a sentence of small menus: **IF** [**Bone** / **Joint angle** / **Rotation**] [which bone] [**Position** / **Speed** / **Acceleration** / **Stillness** / **Relative drop**] [vertical, horizontal, total, X, Y, Z] **from** [the floor, or another bone] **goes** [**below** / **above**] a threshold, with a **margin**. **+ AND condition** adds one; the cross removes one.
  - **Stillness** is how far the bone moves over the next 150 ms, in cm (in ° on an angle). "Stillness below 1 cm" finds the moment a hand or foot settles, even if the motion capture jitters. It comes true slightly before the contact; if it is always early by the same amount, an offset fixes that. On an animation exported in place (the character does not move forward), a planted foot slides back: use **vertical**.
  - **Relative drop** is the bone's speed as a share of its fastest speed over the 300 ms before, in %. "Relative drop below 20 %" finds a stop whether the approach was fast or slow. On one direction only (vertical, X...), the speed also drops where the motion turns back, at the top of a lift: use **total**, or add a height condition.
  - After you change a condition's measure, click **Analyse**: the threshold keeps its old number, which means something else for the new measure.
  - **Bone:** a point on the character, for example the left heel's height above the floor.
  - **Joint angle:** the angle at a joint, such as the knee (180 = straight leg).
  - **Rotation:** how much a bone turns, relative to its parent or to the world.
  - **Threshold and margin:** the event fires when the signal crosses the threshold. It can only fire again after the signal has come back past the threshold by more than the margin (like a gate's hysteresis), so a shaky foot does not give a burst of steps.
  - **The lock icon:** a locked (fixed) threshold is never changed by **Analyse**.
- **Place the marker:** **at the start** of the match, or **at the highest point** / **at the lowest point** of one of the rule's signals. The match is the stretch where all the conditions hold. A contact is best marked at the start (the moment a hand reaches the table); a swing is best marked at the highest point of its speed.
- **Offset**, **Min length**, **Cooldown** (in ms):
  - **Offset** moves every marker of the rule, for example +30 ms if your sound should land after the contact.
  - **Min length**: the match must last at least this long to count.
  - **Cooldown**: no new marker from this rule until this time has passed.
- **Item options** (the gear icon next to **Roles**):
  - **Sensitivity** drops events weaker than this share of the clip's strongest (0 = off).
  - **Edge margin** ignores events this close to the clip's start and end (handy for T-pose frames).

Number fields have no sliders: drag sideways to change them (hold Shift for fine steps), or click and type a value.

### Presets

A preset is a set of rules you can load on any item. The **Preset:** field at the top of the panel shows the item's preset; click it to open the preset menu.

- **Factory** presets come with ReaAnimViewer and update with it. They are read-only. One ships today:
  - **Footsteps**: Smart Detection of steps, combined: an `FS L` / `FS R` marker each time a foot lands.

  Items set up with an earlier **Footsteps Heel** show **Legacy · v3 is installed**: **Update** switches them to Smart Detection (their corrections stay; those made on the old heel rules may show as orphan suppressions or your own events to delete), **Keep v2** keeps their rules. **Footsteps Toe** is gone: items set up with it keep their rules, and the preset field says the preset is gone.
- **User** presets are yours. They live in your own folder, which updates never touch, so you keep them when ReaAnimViewer updates. The bottom of the menu shows that folder, with a button to copy its path.
- **Using the menu:** type in the search field, or hover **Factory** or **User** to see the list. Double-click a preset (or select it and press Enter) to load it on the item under the playhead. Right-click a preset for **Load**, **Rename...** (F2), **Delete** (Del) and **Export...**.
- **Save** (it shows the preset's name) writes the item's current rules over its user preset. **+ Save as...** writes them to a new user preset: type a name and press Enter. A factory preset can't be overwritten: use **Save as**. Overwriting or deleting a preset asks first, inside the menu (in amber): click **Overwrite** or **Delete** (or press Enter) to confirm, **Cancel** (or Esc) to keep it. A deleted or overwritten preset can't be brought back with Ctrl+Z.
- **Import...** copies a `.ravpreset` file into your User presets. **Export...** writes the selected (or loaded) preset to a file you can send to a colleague.

**What an item keeps.** An item keeps its own copy of the preset it was set up with. Changing a preset never changes your items behind your back:

- **edited** next to the preset name means you changed this item's rules or thresholds since loading the preset. That is normal: your thresholds belong to the item. **Save** or **+ Save as...** to turn your changes into a preset.
- **Legacy · v3 is installed** (an amber band) means the preset was updated since this item copied it, for example by a ReaAnimViewer update. Click **Update** to take the new version (it replaces the item's rules), or **Keep v2** to stay on the version you have. After **Keep**, the preset name shows **kept v2** and the band does not come back for that version.

Loading another preset on an item replaces its rules and starts its event list over (your corrections on that item are dropped).

### Roles

Presets don't name bones: they name **roles**. RAV's built-in roles come in three groups:

- **Legs:** left and right heel, toe, toe end, knee and hip (up leg).
- **Arms:** left and right hand, elbow, shoulder (upper arm) and clavicle.
- **Body:** hips, spine, chest, neck and head.

A role is a point: the joint where its bone starts, as **Skeleton** mode draws it. So the knee is the shin bone (`LeftLeg`, `calf_l`), the elbow is the forearm (`LeftForeArm`, `lowerarm_l`), the shoulder is the upper arm (`LeftArm`, `upperarm_l`), and Mixamo's `LeftShoulder` (`clavicle_l`) is the clavicle. When you load a preset, RAV finds the bone for each role from the bone names. It knows Mixamo names (`mixamorig:LeftFoot`, `LeftToeBase`, `LeftHand`...) and the names of Unreal's mannequins (`foot_l`, `ball_l`, `hand_l`, `spine_05`...). When a rig lacks a role's own bone, RAV takes the nearest bone of the same chain: the mannequins have no toe end bone, so their toe (`ball_l`) plays the toe end too, and a rig without `Spine2` (or `spine_05`, `spine_03`) gets its highest spine bone as the chest. A rig without toe ends gets its toe base the same way. The mapping is per skeleton: every animation of the same rig gets the same bones.

- **Roles ✓** (top of the panel) means every role the rules need was found on this skeleton.
- **Roles · 2 missing** (in red) means some roles have no bone on this skeleton. Hover it to see which ones. Only the rules that read them are skipped: each one's row in the strip says **No bone for: ...** in red, its events can't be edited, and its markers stay as the last **Commit** wrote them (a rule never committed has none). The other rules and **Smart Detection** still run, show their previews and are committed as usual. The same goes for an Smart Detection type whose own part has no bone (for example **Grab** with no left hand bone): its earlier markers are kept, never removed. A missing bone never deletes markers already placed. When nothing else can run (every rule that is on misses a role and no Smart Detection type is ticked), the strip shows a message instead of the signals and **Commit** skips the item ("skipped: roles"). Mapping the role brings its rule back at once. **Analyse** still needs every role.
- Click **Roles** to open the **Skeleton & roles** window. It lists every role (under Legs, Arms, Body, then Your roles), the bone that plays each one, and where that bone comes from: `auto` (RAV's guess from the names), `you` (your choice) or `none`. The title says "all found" or how many roles have no bone. The **?** next to the undo arrow sums up the window; **Esc** or the cross closes it.
- To fix a role, open its menu and pick a bone (type in the search field to find it), **— none —** for no bone, or **Auto (...)** to go back to RAV's guess. The strip, **Roles** and **Commit** follow at once.
- Your choice is remembered for every item with the same skeleton, in every project (it is saved in REAPER's resource folder, `ReaAnimViewer/roles.txt`, not in the project). Role changes are saved at once and are not REAPER undo points: while the window is open, **Ctrl+Z** (or the undo arrow next to the close button) undoes them one by one. After you close it, pick **Auto (...)** to go back to the guess.
- You can also pick a raw bone in a condition's bone menu: the **Bones** list under the roles has every bone of the skeleton.
- **Your own roles.** For anything the built-in roles miss (a weapon tip, a tail, a finger), type a name in the **Skeleton & roles** window and click **+ Role** (or press Enter): "Sword Tip" becomes the role **sword tip**, listed for every project. Map it per skeleton like the others, then pick it in a condition's bone menu (it is listed under **Your roles**, after the built-in ones). A preset that uses it works on any rig where the role is mapped: on a new rig, RAV guesses its bone from the bones you picked for it on other rigs (same name, any namespace), shown as `auto`.
- Right-click a role of yours (or hover it and press F2) to rename it: its rules, presets and bones keep working. Its **✕** deletes it (confirmed in place): its bones stay remembered, so creating it again brings them back, and rules that use it keep it. A role read by a rule but not in your list (from a colleague's preset) shows as "not in your roles" and can still be mapped. Creating, renaming and deleting are undone with **Ctrl+Z** like a bone change while the window is open.
- A role of yours made before its name became a built-in role (for example **left hand**) becomes that built-in role: rules and presets keep working, and so do the bones you picked for it. It leaves your list and takes the built-in name.
- **Share your roles.** **Export...** in the **Skeleton & roles** window writes your roles and this skeleton's bones to a `.csv` file that opens in a spreadsheet or a text editor. Pick comma or semicolon separated in the save dialog (semicolon suits Excel in French and other regions). The proposed name is `RAV_Roles_`: add the rig's name after it, since roles belong to a rig, not to one animation. **Import...** reads such a file, yours or one edited by hand, into your roles for the skeleton of the item it is opened on: missing roles are added, the file's bones replace yours, and rows whose bone this skeleton lacks are skipped and counted. **Ctrl+Z** undoes an import. Role names cannot contain `,` or `;`.

### The Tagging view

**V** cycles through **RAV view**, **Tagging view** and **Video view**, or click one at the top of the viewer. The Tagging view uses the same camera as RAV view.

**The strip** (under the character) shows the item under the playhead:

- **One row per rule**, in its colour, with a tick for each event. Your own events are labelled **you**; suppressed ones are faded.
- **Below the rows**, the selected rule's signals, one lane per condition, with the threshold as a line and the margin's re-arm level as a dashed line. **Drag either line** to tune it: the markers follow live.
- **Click or drag** to move the playhead. **Alt+wheel** zooms, **Shift+wheel** scrolls. Drag the strip's top edge to make it taller (more room for the signal lanes).
- **Analyse** proposes this item's thresholds from the clip. It only runs when you click it, and it leaves locked conditions alone.
- **+ Event** (key **E**) adds your own event at the playhead on the selected rule.

**The 3D view** has the **Model | Skeleton** switch at the bottom left (see [Model / Skeleton](#model--skeleton)). In **Skeleton**, the bones the selected rule reads are drawn in the rule's colour, and **+** next to a condition's bone menu puts the selected bone in it (with no bone selected, **+** switches the view to Skeleton).

**The panel** (on the right, drag its edge to resize it): the item's name, **Roles**, the item options, the **Preset:** field, then two tabs of fixed width, each taking the whole height down to the footer:

- **Smart Detection · N on**: the presets, the search and **Is looping**, the type chips (the part that scrolls), then the settings strip (see [Smart Detection](#smart-detection)).
- **Rules · N**: **+ Rule**, the rules list (switch, colour, events, name, **Duplicate**, **Delete**; it scrolls past 6 rules), then the inspector for the selected rule or event of a rule (it scrolls). Missing roles are said at its top.

An amber dot on a tab that is not open: it has something to tell you (a raised sensitivity, a missing role).

**The footer** stays at the bottom, three lines that never move:

- **Detect** · **Commit to N items** · **Cancel**.
- **Populate · N events** · **Reshuffle** · **Repopulate** · **Attach** (once the project has a sound library). **Reshuffle** draws new variations for the placed sounds, at their places; sounds you moved or deleted by hand, and your hand picks, stay. **Repopulate** starts over: it removes the populated sounds (even the ones you moved), forgets the ones you deleted by hand, then places and draws again; attached and spread sounds stay. It asks first, in a bubble over the button ("Remove 37 placed sounds and populate again?"); one undo step.
- The status: the selection, **preview not committed** (or **markers up to date**), the last Commit, the last Populate (click it for the details) and **Restore N** when sounds you deleted by hand wait; what does not fit drops off the line and stays in its tooltip. Then the sounds' **?** (what the marks under the events mean) and **⋯**: **Commit writes** **Take markers**, **Project markers** or **Take + project markers** (for every project), and **Clear item data...** (it asks first, in a bubble; one undo step).

### Correcting events

Detection is never perfect. Fix single events directly in the strip; the rules stay as they are.

- **Click** an event to select it. The inspector shows its time, strength and speed.
- **Drag** an event to a new time: it becomes **yours** (your own event, at a fixed time). Detection never moves your events, even when you change the thresholds later.
- **Double-click** a rule's row to add your own event there, or press **E** to add one at the playhead.
- **Right-click** an event, or select it and press **Delete**:
  - a detected event is **suppressed**: it is masked and gets no marker (any detection of that rule within 30 ms of that time stays masked);
  - a suppressed event is **restored**;
  - your own event is **deleted**.

  The inspector's button does the same (**Suppress**, **Restore** or **Delete**). **Go to event** moves the playhead to it.

When the animator delivers a new animation on the same item, RAV detects it again from scratch and your corrections on that item are dropped.

### Markers

**Which markers:** in the viewer menu's **Tagging view** tab, **Markers written**: **Take** (markers on the item), **Project** (markers on the timeline, in the rule's colour) or **Both** (the default). The panel footer's **⋯** sets it too (**Commit writes**). It applies to every project.

**Preview, Commit and Cancel:**

- Every change you make (a threshold, a rule, a preset, a correction) is shown at once as **preview markers**, named `<marker> - Preview`, in a darker colour, next to the markers you committed. They disappear when the result is the same as the committed one.
- **Commit to N items** writes the real markers of every **selected** item that has rules, each with its own rules, and removes their previews. Items linked to a selected one (see [Copies of one animation share their tagging](#copies-of-one-animation-share-their-tagging)) are committed too: the footer counts them as "linked". It replaces only the markers RAV wrote: your own markers are never touched. If one of your markers already has the same name at the same time, RAV leaves it and writes no duplicate. Items without rules are skipped and the footer says so.
- **Cancel** removes the previews of the selected items (and of the items linked to them) and puts their rules, thresholds and events back as they were at their last Commit.
- The button reads **Nothing to commit** when the selected items' markers are up to date. The footer line says **markers up to date**, **preview not committed** or **markers not written yet**.

Every change, Commit and Cancel included, is saved in the project at once and is one step in REAPER's undo history. There is no separate "save" step.

### Markers follow their item

RAV's project markers stay attached to their item, even with the RAV window closed:

- **Move** or **trim** the item: its markers move with it. A marker whose event falls outside the trimmed item disappears, and comes back when you extend the item again.
- **Split** an item: each part gets the markers of its own part.
- **Duplicate** or copy-paste an item: the copy gets its own markers (and shares the original's tagging, see below).
- **Delete** the item: its markers are deleted too.

Take markers always follow their item, as in any REAPER item.

### Editing the markers in REAPER

You can also correct events right on REAPER's timeline, with the RAV window open or closed. The item's event list follows your edit:

- **Drag** a RAV marker (take or project): it becomes your own event at the new time, as if you had dragged it in the strip.
- **Delete** a RAV marker: its event is suppressed (or removed, if it was your own event).
- **Rename** a RAV marker, or drag it outside its item: the marker becomes yours. RAV leaves it where it is, never moves or deletes it, and suppresses its event.
- With **Both**, the other marker of the same event follows (drag one, the other moves; delete one, the other goes).
- After editing a committed marker, the item still reads **markers up to date**: no preview appears. Ctrl+Z undoes the marker edit and the event change together.
- The items linked to it follow too (see the next section).

### Spreading your sounds onto the markers

Cut a footstep (one item, or a stack of layers) and its variations, then tell RAV "these sounds, on every footstep". It works like **Populate**, except that RAV takes the sounds you selected instead of a library you picked. It works for any marker: a foot, a hand, the body, a knee, your own markers.

1. Select the sound items. Items that overlap in time are one variation and its layers; items apart in time (or touching end to end) are separate variations. An item holding its variations as takes (NVK style) is one Sound whose takes are its variations. A selected NVK folder item is one variation holding its child items: when its layers hold their variations as takes, every placement plays the same take on all its layers.
2. Click the label of one RAV project marker of the kind you want, for example `FS L`. With several RAV markers selected, the first in time is used and the report says so. With **Markers written** set to **Take** only, there is no project marker to select: the action does nothing and says so.
3. Run **RAV: Tagging - Attach selected items to selected marker (this animation)** for every item of this animation (the same animation name, linked copies included), or **(all animations with this marker)** for every animation item that has that marker.

**Both sides.** The marker's kind is taken with its sides: selecting an `FS L` marker also gives the sounds to the `FS R` markers, and the reverse. RAV reads the side from the rule that writes the marker, not from its name. A kind with no side (a body marker, or a rule of your own) is that kind alone.

RAV turns your selection into one library per kind, named without its side (`FS`), listed under **Attached** in the Library view: a Random of your variations, a layered variation being a Blend of its layers. Each sound is placed by its snap offset: the snap point lands on the marker, and the other layers keep their place under the top one. The library is given to the rules that write that kind (both sides), then Populate places it: every marker gets a variation drawn at random (avoiding the last two, also from one animation item to the next), takes included. Only those rules are populated: your other libraries are left as they are. The sounds follow the animation item and its markers like any populated sound.

Your first selected variation (with its layers) is not copied: it moves onto the marker you selected, its snap point on it, and stays attached to that marker. It follows the marker, Populate leaves that marker alone, and the copies on the other markers avoid repeating it. The other selected items (and the child items of a selected folder item) move after the end of the project, side by side as you cut them. All of them are the library's sources, so keep them. The result shows at the bottom right of REAPER's window, and Ctrl+Z undoes the whole run (library, assignment, moved items, placed sounds).

- **Run it again.** Run the action again with a new selection on the same kind: the same `FS` library gets the new sounds in place of the old ones, for this animation (every item of it) or for every animation, and those markers are populated again (your hand edits are kept, as with Populate). The sounds an earlier run (or you, by hand) attached to these markers are never deleted: they move after the end of the project with the other sources, and the report counts them. You can edit the library like any other (rename it too: it stays the `FS` library, reshuffle, change a variation).
- **Variations, takes and tracks.** An item whose take has take markers (2 or more) gets one variation per marker (Detection **Take markers**); an item without them is cut on silence (as a new Sound in the Library: -60 dB, 200 ms gap): each region of sound is a variation, wherever it sits in the file. Either way, each variation's snap point (the S of Sound Edit) is found on its own sound (the Sound's Sync, Attack or Peak), never copied from where your original sits against its marker, and every copy is as long as your original and cut around that snap as your original is around its snap offset, so its snap lands on the marker. The zones show in Sound Edit at once. An NVK folder pile takes its variations the same way from its top item (its take markers, else its regions of sound), but its copies are not cut to your original's length: each layer plays its whole variation of that number. An item with several takes keeps its takes as its variations. The layers of a stack play the same variation number (region or take 2 of one layer goes with 2 of the others), with or without the NVK folder item. The copies land on your source items' own tracks, where you cut them: RAV never makes a track or a folder for them and never moves your items (copies that overlap simply overlap there).

#### Adding a layer (Add to pile)

Cut the new layer's variations and put the first one where you want it against an attached sound (for example 20 ms after the footstep's snap point). Select the new items with the attached sound: the original or any copy RAV placed for it. Several linked instances of that sound may be selected with them: the one nearest the new items is used. Sounds of two different layers (or of two markers that are not linked): RAV refuses and changes nothing. No marker to select. Then run one of:

- **RAV: Tagging - Add selected items to stack (all animations with this marker)**: Add to pile with **Apply to Project** (below): the layer goes on every instance of the kind, in every animation.
- **RAV: Tagging - Add selected items to stack (this animation)**: Add to pile with **Apply to Anim**: the layer goes on every instance of this animation (every item with the same animation name, linked copies included); the other animations are unchanged.

The layer keeps the offset you gave it (measured between snap points). The new items never include the pile's own sounds (its folder item, the library's sources). The new variation you put against the sound stays there, attached to that marker; the others go after the end of the project with the other sources. Ctrl+Z undoes it in one step.

#### The Attach manager window

**RAV: Attach manager (at mouse)** opens a small window at the mouse pointer, outside RAV's panel (give it a key or a toolbar button in REAPER's Actions window). It opens wholly on screen with the pointer on its title row, stays open and follows REAPER's time selection, item selection and Ctrl+Z; drag its title row to move it, Esc or ✕ closes it. The four actions above keep working; they are its Attach and Add to pile with **Anim** or **Project**.

Every sound Attach or Add to pile places is an **instance**: the copies on the markers and the original you put there alike. Each instance draws its variation (takes, take markers) as Populate does, and has the original's length and window.

- **Apply to**: **Item**, **Anim** or **Project** says how far each gesture goes: **Item** = this instance only; **Anim** = every item of this animation (the same animation name, as RAV shows it, linked copies included); **Project** = every instance of these markers in the project. It is a setting of the project (Anim at first, one undo point when you change it), and it also applies to what you do in REAPER with the window closed (see below).
- **Marker**: make a time selection over the markers you want. The list holds one entry per marker kind and side in it (`FS L`, `FS R`, `Scuff L`...), each at its first marker in time, plus the marker label you clicked, if any. It picks your choice, else the clicked label, else the first in time. **On** names its animation.
- **Selection**: **Attach** or **Add to pile**, chosen from the item selection (an attached sound plus new items: Add to pile on that sound's marker; switch back with the two buttons). The line under it says what will happen, and the blue button runs it (**Attach · Anim**): one undo point, the report at the bottom right of REAPER. When it cannot run, it is greyed and the reason shows above it.
  - **Attach** puts the selected sound on the marker and, with **Anim** or **Project**, on every marker of that kind in scope: L and R together, or only the marker's side once L and R are separated. It replaces the sound those markers had (the previous original goes after the end of the project). With **Item**, only that marker gets it, nothing is copied elsewhere and the library is unchanged.
  - **Add to pile** adds the new items as a layer to every linked instance in scope, at the same distance from the marker. With **Item**, only that instance gets it.
  - **Reshuffle** draws the variation of the selected copies again (the copies RAV placed, not your original), as RAV's Reshuffle draws, and leaves every other sound as it is: one undo point. Greyed when no copy is selected.
- **L and R** says whether the marker's kind plays one sound on both sides (**Together**) or two (**Separate**), in the marker's animation. **Separate L and R** (Anim or Project; greyed with Item): L and R keep the same sound but become two groups: adding to the pile and deleting then reach one side only. **Merge L and R (keep the L sound)**: both sides play the chosen marker's side's sound again; the other side's sounds are replaced.

**In REAPER, with the window open or closed**, deleting an instance deletes its linked instances in **Anim** or **Project** (as **Apply to** says; with **Item**, only that one): its layer is gone from every linked instance and from the library in scope. An original a library still plays from is never lost: it goes after the end of the project with the other sources. Nothing else follows: clicking an instance selects only it, and its volume, fades and position are its own. The delete of the linked instances is one RAV undo point right after yours (Ctrl+Z twice puts both back).

RAV keeps this in the kind's one `FS` library, under Switches it manages: **RAV - FS animations** (one value per animation, shared by all its items, the default value for the others) and **RAV - Side** (L and R, once separated). It never makes a track or a folder, never moves an item other than the sounds concerned, and never touches another marker kind.

### Syncing placed sounds with the Library

Changing a setting in the Library (a node's volume, a Blend offset, a Sound's fades, sync point, zones or level, a variation switched off) never moves anything in the arrange: the sounds already placed keep what they had. The Library shows what is behind:

- At the top of the tree, the **Placed sounds** card says how many changes of the library shown are not yet on its placed sounds ("1 change not yet on 26 placed sounds in 3 items"). Each library has its own card. When everything matches it folds into a green line, **Placed sounds match the library**.
- The nodes whose placed sounds are behind carry a **↻ N** mark (N placed sounds); hover it for what changed. The node panel says it too, with a **Sync this node…** button.

**Sync…** (or a **↻** mark: the sounds of that node, with every change they are behind on) opens the choice of where: the animation items holding sounds behind, by track, each with its sounds and the nodes they play. Check the items (per item, a whole track, **Selected in REAPER**, **All** / **None**), then **Apply to N items** or **Apply to all**. An item left unchecked keeps the old setting and stays counted on the card.

Sync keeps the draw: each placed sound keeps its variation, track and position, and RAV updates what changed, in place (your take FX and other edits on the item stay). Only a sound whose variation was switched off is drawn again, among the variations still on; with none left on, it is left as it is. A change of structure (a node added, a Switch value, an assignment) is not Sync's: **Populate** handles it. **Repopulate** (Tagging footer) does not change: it removes and draws again.

**Sounds you edited by hand.** A placed sound whose volume, fades, position or length you changed in REAPER is listed in a second dialog (the item, its time, what you changed, its node). For each one, or for all of them: **Keep** leaves your change and updates the rest (your volume change stays on top of the library's new volume), **Overwrite** puts the library's settings back (a moved sound goes back to its place). **Don't ask again** remembers the answer when it is the same for all of them (greyed when the choices differ). The rule is per project, in ☰ › Tagging view › Sounds › **Hand-edited placed sounds, on Sync**: Ask each time, Keep my edits, or Overwrite them.

One Sync is one undo point, "RAV: Sync placed sounds" (the remembered rule included); the toast says how many sounds were adjusted, redrawn, kept, overwritten and skipped (left as they are: no variation left on, or a change only Populate makes). Sounds placed by a RAV older than this feature are not counted until their next Populate.

### Copies of one animation share their tagging

Sound designers often duplicate one animation to build a longer take: a two-step walk copied five times makes ten steps. You tag it once. Like REAPER's pooled MIDI items, the items of a project that play the same animation file are **linked**: they share one tagging (rules, thresholds, item options, preset, events and corrections).

- **One change, every copy.** A threshold drag, a rule change, **Analyse**, loading, saving or updating a preset, an event correction: every linked item takes it at once, with its preview markers, in ONE undo step. Ctrl+Z puts every copy back together.
- **Each copy on its own place.** The events are kept in animation time; each item places them on its own position, trim and play rate. A copy trimmed to the second step shows only the events of that step.
- **Commit and Cancel** act on every linked item of the selected ones, even those not selected.
- **Edits in REAPER** reach the copies too: drag, delete or rename a RAV marker of one copy, and the event changes on every copy; their markers follow at once and stay **markers up to date**.
- **The panel** shows the link under the item's name: **Linked · 4 other copies**. Hover it for what is shared.
- **Make unique** (the button on that line, or the action **RAV: Tagging - Make tagging unique (selected items)**) gives an item a tagging of its own, as it is now: from then on, a change on it no longer reaches the other copies, nor theirs this one. The line then reads **Not linked**, with a greyed **Unique**. Use it for a deliberate variation (a limp on one step). Duplicate a unique item and the duplicate is linked to it. To link an item back, undo the Make unique (Ctrl+Z): there is no "link again" button yet.
- What makes copies: the same animation file (the same path, whatever its case or slashes), in the same project. Two different files never link, even with the same motion, and items in different project tabs never link. An item relinked to another file leaves its old copies and joins the new file's.
- An item added later from the same file (no tagging of its own yet) is linked too: its panel shows the shared tagging at once, a change made on it changes the shared tagging, **Commit** writes its markers like the others', and **Make unique** keeps the shared tagging as its own.
- Copies tagged differently by an earlier build are linked as well: the next change on one of them gives its tagging to every copy (Ctrl+Z undoes it).

### Known limits

- Smart Detection covers the legs (the feet's steps, lift-offs, slides and pivots, jumps and landings, taps and drags, heel taps, kicks, the knees on the ground), the arms (the hands' grabs, releases and pivots, slaps, claps and hands on the body, swings and punches, the elbows on the ground) and the body (its jolts and spins, sitting down, a fall onto the ground) today. Every other event is tagged with your own rules (see [Tagging your own events](#tagging-your-own-events)), which you can save and share as presets. Punch was set on the sample punches (straight punches, jabs and crosses); Kick on two capoeira kicks and synthetic motions, so check their markers by ear.
- Smart Detection: a touch shorter than about a tenth of a second is not a contact, a foot held still in the air reads as planted, and an animation exported in place (walking forward, or climbing) is read right only when a foot is planted for more than half of the clip. A slide or pivot gets one marker, at its start.
- Smart Detection of pivots on a steep foot: on tiptoe, RAV reads the turn from the toe end, or, on a rig without one, from the toe bone's rotation (as long as the foot stands flat on its ball somewhere in the clip). On a rig with a toe end, a turn on the heel with the toes raised almost vertical (about 80°) is lost; with the toes lower, the heel and the toe still read it.
- Smart Detection of the hands was set on a single tagged grab (a climb), so check its markers by ear. A hand that freezes in the air after a fast move (a long dance hit) reads as a grab.
- Smart Detection of a turn that ends as the foot leaves the ground: under 30° (by default) it gets no pivot, even when you hear a scuff; raise the pivot sensitivity to catch it.
- Smart Detection of hand pivots reads the hand's turn against how the hand rests over the whole clip: a hand that rests palm-down in one contact and flat on a wall in another may get no hand pivot.
- Smart Detection of hand pivots has no tagged reference yet: it borrows the foot pivot's settings (how fast and how far the turn goes). Check its markers by ear.
- Smart Detection of knees, elbows, sitting and falls has no tagged reference yet: it was set on the kneeling, crawling and falling sample clips (a knee rests about 10 cm above the floor, its joint's centre). No sample clip sits down or rests on its elbows, so check those markers by ear.
- Smart Detection of claps and hands on the body has no tagged reference yet: no sample clip claps, and only a few touch the body. It reads the hand bone (the wrist) against the other wrist or the body's bones, so a touch on the far side of a thick body part (the belly, the back) can be missed. Check its markers by ear.
- Roles are guessed from the bone names only (Mixamo and Unreal-style names). For another naming, set them once per skeleton in the **Skeleton & roles** window (click **Roles**).
- In RAV view, hovering a joint in **Skeleton** shows its roles only once the Tagging view has been opened on an item with this skeleton.
- The 3D view does not follow a take's play rate or a looped item, while the strip and the take markers do: on such items the pose and the markers can disagree.
- Commit skips an item that has no rules left, and leaves the markers it wrote earlier: delete them in REAPER if you remove all of an item's rules.
- A unique item can only be linked back to its copies by undoing its **Make unique**.
- Two items that would write the same project marker at the same time (for example a character and its weapon on two tracks) share one marker.
- Detection reads the bones' motion only: an animation built at runtime in the engine (IK, blend trees) must be baked into the exported file, as for viewing.

---

## Saving and Sharing Projects

- The viewer state (items, docked window position) is saved with your REAPER project and restored when you reopen it.
- To send a session to a colleague, use **File > Save project as...** with **Copy all media into project directory**: the animation files are copied with the audio, and the project opens on another machine without re-importing anything.
- When the animator delivers a new export, replace the file on disk (same name) and reopen the project: your items and sounds stay in place.

---

## Exporting Animations from Your Engine

See [Exporting animations from Unreal Engine 5](docs/EXPORTING_ANIMATIONS_FROM_UNREAL.md) for a step-by-step guide to producing a clean `.glb` (mesh + textures + one animation).

**Rule of thumb:** always export as **glTF binary (`.glb`)** when your engine or DCC supports it.

---

## Building from Source

Only needed if you want to contribute. Users should install through ReaPack.

**Prerequisites:** Windows x64, Visual Studio 2022 (Desktop development with C++), CMake 3.20+, Git.

```sh
git clone https://github.com/DemuteTools/ReaAnimViewer.git
cd ReaAnimViewer
build.bat
```

`build.bat` compiles the extension and the video FX and installs them into REAPER (close REAPER first). Output: `build/Release/reaper_animviewer.dll` and `build/Release/rav_video_fx.clap`. Third-party dependencies are either vendored in `extern/` or fetched and statically linked by CMake (see [`extern/VENDORED.md`](extern/VENDORED.md)).

Build options, releases and the ReaPack workflow are documented in [`MAINTAINING.md`](MAINTAINING.md).

---

## License

MIT, see [`LICENSE.md`](LICENSE.md).

Released free for the game audio community by [Demute Studio](https://www.demute.studio/).
