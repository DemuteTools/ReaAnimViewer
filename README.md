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
  - [Auto detection](#auto-detection)
  - [Tagging your own events](#tagging-your-own-events)
  - [Rules](#rules)
  - [Presets](#presets)
  - [Roles](#roles)
  - [The Tagging view](#the-tagging-view)
  - [Correcting events](#correcting-events)
  - [Markers](#markers)
  - [Markers follow their item](#markers-follow-their-item)
  - [Editing the markers in REAPER](#editing-the-markers-in-reaper)
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
7. Open the viewer with the action **RAV: Open Viewer**, or with **Run** on the card.

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
7. Open the viewer with the action **RAV: Open Viewer** (**Actions > Show action list**, search for **RAV**).

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
4. Start REAPER and look for **RAV: Open Viewer** in the action list.

A manual install does not update itself: repeat these steps for each new version.

### Troubleshooting

| Problem | Fix |
|---------|-----|
| **RAV: Open Viewer** does not appear in the action list | Restart REAPER after installing. Make sure you run the 64-bit Windows version of REAPER. With the Toolkit, make sure you clicked **Run** on the card once before restarting. |
| **Run** on the Toolkit card does nothing at all | Update the **ReaAnimViewer** card in the Toolkit (0.2.0 had this bug when a toolbar button for **RAV: Open Viewer** already existed), then click **Run** again. |
| **Run** says "ReaPack still lists ReaAnimViewer as installed" | The extension was once installed with ReaPack and its file was deleted by hand, so ReaPack still thinks it is installed. In the ReaPack window that opens, right-click the **ReaAnimViewer** package named in the message, choose **Uninstall**, click **Apply**, then click **Run** again. |
| **Run** says ReaAnimViewer is installed, but after a restart **RAV: Open Viewer** is still missing | Same cause as above with an older launcher: update the card first. If it persists, go to **Extensions > ReaPack > Browse packages...**, uninstall every installed **ReaAnimViewer** entry, click **Apply**, then click **Run** on the card and restart REAPER. |
| The viewer opens but stays black / shows an error | Update your graphics driver: the viewer needs OpenGL 3.3. |
| ReaPack reports a conflict on `reaper_animviewer.dll`, or the extension stays on 0.1.x | See [Upgrading from 0.1.x](#upgrading-from-01x). |
| ReaPack says the file is in use when updating | Close REAPER, reopen it, and run **Synchronize packages** again before loading a project. |
| The viewer shows **Can't load ...** (red) | The file can't be opened. The message says why and what to change in the export (for example a missing mesh, or an FBX saved as text). |
| The viewer shows **... has problems** (orange) | The file opened, but something is missing: textures, the animation, or the skin. The message says what to fix. It pops up once per file. |
| The message is gone and you want to read it again | While the current item has a problem, a red or orange **warning icon** stays in the bottom-left corner of the viewer. Hover it to see the message again. |
| You need to report a problem to us | Click **Copy details** next to the message (or **Copy error log** in the viewer menu) and paste the text into your bug report. |

---

## Getting Started

1. Open the viewer: **Actions > Show action list**, search **RAV**, run **RAV: Open Viewer**. Dock it wherever you like, REAPER remembers the position.
2. Drag a `.glb`, `.gltf` or `.fbx` file from Windows Explorer onto a track. An item is created with the exact length of the animation.
3. Move the playhead over the item: the character appears in the viewer, posed at that frame.
4. Press **Play**. The animation runs in sync with the transport: scrub, loop and stop as you would with a video.
5. Right-click and drag in the viewer to turn around the character. Start placing your sounds.

**Tip:** bind **RAV: Open Viewer** to a key or toolbar button to toggle the viewer quickly.

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

The tab follows the view when you change view (toggle or **V**); otherwise the tab you picked stays. The **Change** button in the Tagging view's panel opens it on the **Tagging view** tab. On a short viewer the settings scroll; the tabs stay in place.

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

Click the **keyboard icon** in the top-right corner of the viewer (left of the FPS readout) to see every key and mouse gesture. They work while the viewer has the focus; every other key stays REAPER's. The default keys:

| Key | Action | Where |
|-----|--------|-------|
| **V** | Next view: RAV / Tagging / Video | Anywhere |
| **P** | Show / hide the Video panel (from RAV view or Tagging view it also switches to Video view) | Anywhere |
| **C** | Cut at playhead | Video view |
| **Delete** | Delete current shot | Video view |
| **Ctrl+C** | Copy shot camera | Video view |
| **Ctrl+V** | Paste shot camera | Video view |
| **E** | Add event at playhead | Tagging view |
| **Delete** | Suppress / delete event | Tagging view |

- **Double-click** a key to change it, then press the new key. Ctrl, Shift and Alt can be held with it. Esc cancels. A key already used by another RAV action is refused.
- **Right-click** a key to restore its default.
- Mouse gestures are listed for reference and cannot be changed.
- **Esc** (or a click outside it, or the icon again) closes the popup.

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
2. The panel follows the track of the animation shown in the viewer. When it says **No video FX on this track**, click **Add video FX to track**. The action **RAV: Add video FX to selected track** does the same from REAPER.
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
| REAPER's Video window stays black | Open it with **View > Video**. A video item on a track above can cover the FX's picture. To check that the FX works at all, turn on the action **RAV: Video FX test pattern**: an orange frame with a moving bar means the FX and the extension talk to each other. Turn it off again. |
| The rendered video has the wrong size | Check **Output > Size** in the panel, and the video size in the render format's settings. |

To report a problem, click **Copy error log** in the viewer menu and paste it into your report.

---

## Auto-Tagging (Event Markers)

ReaAnimViewer can find events in an animation from the way its bones move, and mark them on your timeline: footsteps, a hand hitting something, a sword swing, a body falling, a head turn, a clap... Feet, and hands that grab or let go, are found for you by **Auto detection**. For anything else you can describe as "this bone goes above or below this value", you write **rules** (or load a preset). You check the result in the **Tagging view**, and commit. Each event becomes a marker named after its rule, on the item (take marker), on the timeline (project marker), or both. Then place your sounds on the markers instead of hunting for each frame by eye.

Footsteps are the ready-made starting point: under **Auto detection**, add **Feet**, tick **Step** and press **Detect** (or load the **Footsteps** preset, which does the same). Lift-offs, slides and pivots of the feet, and the hands' grabs, releases and pivots, work the same way. For everything else, you build your own rules, then save them as presets to reuse them.

Each animation item keeps its rules, thresholds and corrections, saved in the project. Items that play the same animation file share them, like REAPER's pooled MIDI items (see [Copies of one animation share their tagging](#copies-of-one-animation-share-their-tagging)). Nothing is written to the animation file.

### Quick start: footsteps

1. Open the viewer, then click **Tagging view** at the top (or press **V** until it shows). A strip appears under the character and a panel on the right.
2. Put the playhead over an animation item. At the top of the panel, under **Auto detection**, click **+** and choose **Feet**, then tick **Step** and press **Detect**.
3. The strip shows one row per foot (`FS L`, `FS R`) with a tick for each step found. Play or scrub to check them against the character.
4. Too many or too few? Drag the step's **Sensitivity** (higher finds more). Always a little early or late for your sounds? Drag its **Offset**. The markers follow as you drag. Then correct single steps if needed (see [Correcting events](#correcting-events)).
5. As soon as you change something, RAV shows the result on the timeline as **preview markers** (`FS L - Preview`, in a darker colour). Nothing is final yet.
6. Select the items to tag in REAPER (the panel footer says how many), then click **Commit to N items**. The previews are replaced by the real markers. **Cancel** instead puts the items back as they were at their last Commit.

To tag several walk cycles at once, set them up one by one (steps 2 to 4), select them all and click **Commit** once: each item is tagged with its own rules. Copies of one cycle (the same file duplicated to build a longer walk) are set up once: they share their tagging.

### Auto detection

Auto detection reads the body as physics: it measures the legs to know the character's size, finds where each part of the foot, and each hand, comes to rest (a floor, a stair, a ledge, a hold, a table), and places the markers from that. One setting works on every clip: there are no thresholds to tune, and no **Analyse**.

- The event types are grouped in categories, **Feet** and **Hands**. The section shows only the categories the item uses: click **+** to add one. Click a category's name to fold or unfold it; it shows how many of its types are ticked (`2 on`). Its **×** unticks all its types: press **Detect** to remove them. A category with detected types stays until then, with `0 on`; one added with **+** and never detected goes at once.
- **Tick** the event types you want, then press **Detect** (one button for every category):
  - **Feet**
    - **Step**: where the foot lands. **Combined** gives one marker per landing (`FS L`, `FS R`), at the first part of the foot to touch. **Separate** gives the heel and the toe their own markers (`Heel L`, `Toe L`, `Heel R`, `Toe R`). The toe is the ball of the foot (the toe bone), or the toe tip on a rig without a toe bone. A foot that glides into place (high heels on a catwalk) gets its step where it stops.
    - **Lift-off**: where the foot leaves the ground (`Lift L`, `Lift R`).
    - **Slide scuff**: where a planted foot slides on the ground (`Slide L`, `Slide R`).
    - **Pivot scuff**: where a planted foot turns on its ball or its heel (`Pivot L`, `Pivot R`): flat, on tiptoe with the heel up, or on its heel with the toes up. A foot that turns as it rolls onto its toes and stays there (a swivel) pivots; one that leaves the ground at once (a heel whip at toe-off) does not.
  - **Hands**
    - **Grab**: where the hand lands on something and stays: a hold, a ledge, a table, the floor (`Grab L`, `Grab R`). A hand that only slows down, or stops for an instant between two moves, is no grab.
    - **Release**: where the hand leaves what it held (`Release L`, `Release R`).
    - **Pivot scuff**: where a planted hand turns on what it rests on, a hand on the floor or a ledge while the body swings round it (`Hand Pivot L`, `Hand Pivot R`). The turn is the hand's own twist: a vault where the palm stays put gives no marker. A hand that lands turning, or leaves at once, does not pivot. **Only after a Grab** (on by default): only a hand that came down with a grab (or rests from the start of the clip) can pivot. Off: any resting hand can, a hand placed gently too, but a hand that twists while held still in the air can then give an extra marker.
- **Detect** lights up while a change waits for it: a type ticked or unticked, steps switched between **Combined** and **Separate**, or the hands' **Only after a Grab** switched. It writes them in one undo step. Each ticked type adds one row per foot (or per hand) to the strip, next to the rules' rows.
- **Sensitivity** (%, one per type): higher finds more events, lower fewer; 50 % is the default. **Offset** (ms, one per type) moves every marker of that type, for example +20 ms if your sound should land after the contact. On a type already detected, both apply at once: the markers follow while you drag, and releasing is one undo step.
- Correct single events as on any row (see [Correcting events](#correcting-events)): your corrections survive a new **Detect** and a sensitivity change. After an offset change, check them: a suppressed event stays masked only while the detection is within 30 ms of where it was, and your own events stay where you put them. Unticking a type removes its rows and the corrections on them.
- Click a row's name in the strip to select it: the inspector shows its marker name and colour, which you can change there. Its type is set in the **Auto detection** section.
- **Roles:** auto detection needs, for at least one leg, the heel, the knee and the hip (up leg; the hips will do), or else the knee and the hip (up leg) alone; per foot the heel and the toe (or the toe end); per hand its hand bone. When a ticked type misses one, the section names it in red and that foot or hand gets no events of that type: map the bone in **Roles**. A rig without foot bones still gets its hand events, as long as the knee and the hip (up leg) of one leg are mapped.
- Auto detection and rules work together on one item: the rules list shows only the rules, and both make markers. A preset saves both.

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
  - **Footsteps**: auto detection of steps, combined: an `FS L` / `FS R` marker each time a foot lands.

  Items set up with an earlier **Footsteps Heel** show **Legacy · v3 is installed**: **Update** switches them to auto detection (their corrections stay; those made on the old heel rules may show as orphan suppressions or your own events to delete), **Keep v2** keeps their rules. **Footsteps Toe** is gone: items set up with it keep their rules, and the preset field says the preset is gone.
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
- **Roles · 2 missing** (in red) means some roles have no bone on this skeleton. Hover it to see which ones. Only the rules that read them are skipped: each one's row in the strip says **No bone for: ...** in red, its events can't be edited, and its markers stay as the last **Commit** wrote them (a rule never committed has none). The other rules and **Auto detection** still run, show their previews and are committed as usual. The same goes for an Auto detection type whose own part has no bone (for example **Grab** with no left hand bone): its earlier markers are kept, never removed. A missing bone never deletes markers already placed. When nothing else can run (every rule that is on misses a role and no Auto detection type is ticked), the strip shows a message instead of the signals and **Commit** skips the item ("skipped: roles"). Mapping the role brings its rule back at once. **Analyse** still needs every role.
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

**The panel** (on the right, drag its edge to resize it): the item's name, **Roles**, the item options, the **Preset:** field, the **Rules** list, then the inspector for the selected rule or event. The footer has **Commit**, **Cancel**, which markers are written, and a summary of the selection.

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

**Which markers:** in the viewer menu's **Tagging view** tab, **Markers written**: **Take** (markers on the item), **Project** (markers on the timeline, in the rule's colour) or **Both** (the default). The panel footer shows the current choice; **Change** opens the menu on that tab. It applies to every project.

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

### Copies of one animation share their tagging

Sound designers often duplicate one animation to build a longer take: a two-step walk copied five times makes ten steps. You tag it once. Like REAPER's pooled MIDI items, the items of a project that play the same animation file are **linked**: they share one tagging (rules, thresholds, item options, preset, events and corrections).

- **One change, every copy.** A threshold drag, a rule change, **Analyse**, loading, saving or updating a preset, an event correction: every linked item takes it at once, with its preview markers, in ONE undo step. Ctrl+Z puts every copy back together.
- **Each copy on its own place.** The events are kept in animation time; each item places them on its own position, trim and play rate. A copy trimmed to the second step shows only the events of that step.
- **Commit and Cancel** act on every linked item of the selected ones, even those not selected.
- **Edits in REAPER** reach the copies too: drag, delete or rename a RAV marker of one copy, and the event changes on every copy; their markers follow at once and stay **markers up to date**.
- **The panel** shows the link under the item's name: **Linked · 4 other copies**. Hover it for what is shared.
- **Make unique** (the button on that line, or the action **RAV: Make tagging unique (selected items)**) gives an item a tagging of its own, as it is now: from then on, a change on it no longer reaches the other copies, nor theirs this one. The line then reads **Not linked**, with a greyed **Unique**. Use it for a deliberate variation (a limp on one step). Duplicate a unique item and the duplicate is linked to it. To link an item back, undo the Make unique (Ctrl+Z): there is no "link again" button yet.
- What makes copies: the same animation file (the same path, whatever its case or slashes), in the same project. Two different files never link, even with the same motion, and items in different project tabs never link. An item relinked to another file leaves its old copies and joins the new file's.
- An item added later from the same file (no tagging of its own yet) is linked too: its panel shows the shared tagging at once, a change made on it changes the shared tagging, **Commit** writes its markers like the others', and **Make unique** keeps the shared tagging as its own.
- Copies tagged differently by an earlier build are linked as well: the next change on one of them gives its tagging to every copy (Ctrl+Z undoes it).

### Known limits

- Auto detection covers the feet (steps, lift-offs, slides and pivots) and the hands' grabs, releases and pivots today. Every other event (a hand swing, a clap, a punch, the body) is tagged with your own rules (see [Tagging your own events](#tagging-your-own-events)), which you can save and share as presets.
- Auto detection: a touch shorter than about a tenth of a second is not a contact, a foot held still in the air reads as planted, and an animation exported in place is read right only when a foot is planted for more than half of the clip. A slide or pivot gets one marker, at its start.
- Auto detection of pivots on a steep foot: on tiptoe, RAV reads the turn from the toe end, or, on a rig without one, from the toe bone's rotation (as long as the foot stands flat on its ball somewhere in the clip). On a rig with a toe end, a turn on the heel with the toes raised almost vertical (about 80°) is lost; with the toes lower, the heel and the toe still read it.
- Auto detection of the hands was set on a single tagged grab (a climb), so check its markers by ear. A hand that freezes in the air after a fast move (a long dance hit) reads as a grab.
- Auto detection of hand pivots reads the hand's turn against how the hand rests over the whole clip: a hand that rests palm-down in one contact and flat on a wall in another may get no hand pivot.
- Auto detection of hand pivots has no tagged reference yet: it borrows the foot pivot's settings (how fast and how far the turn goes). Check its markers by ear.
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
