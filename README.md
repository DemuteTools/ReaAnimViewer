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
  - [Preview lag while playing](#preview-lag-while-playing)
  - [If the video FX shows nothing](#if-the-video-fx-shows-nothing)
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
| **Recenter camera** (viewer menu) | Frame the character again |

---

## Viewer Menu

Click the **menu icon** in the top-left corner of the viewer to show or hide the tool menu. It has two groups: **View** (Light, Ground, Shadow, Performance, below) and **Tools** (**Video** shows or hides the Video panel, see [Rendering Video](#rendering-video)).

- **Recenter camera**: Frame the character again.
- **Copy error log**: Copy the recent load errors, the viewer version and your graphics card info to the clipboard, ready to paste into a bug report.

The installed version is shown at the bottom of the menu.

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
| **REAPER preview lag** | Show how far REAPER's Video window runs ahead while playing (Video view, see [Preview lag while playing](#preview-lag-while-playing)) |

If the viewer feels slow on a laptop, lower **MSAA** and **Shadow** first.

---

## Shortcuts

Click the **keyboard icon** in the top-right corner of the viewer (left of the FPS readout) to see every key and mouse gesture. The default keys are **V** (RAV view / Video view), **P** (show / hide the Video panel) and **C** (cut at the playhead, in Video view). They work while the viewer has the focus; every other key stays REAPER's.

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

1. Open the viewer and press **V** (or click **Video view** at the top). The **Video panel** opens on the right. **P**, the panel button at the top right, or **Tools > Video** in the viewer menu show or hide it.
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

- **Shot strip** (under the viewport, in Video view): the shots over the current item, and REAPER's playhead as a white line. Click or drag in it to move the playhead. Drag a line between two shots to retime that cut: it snaps to frames, each shot keeps at least one frame, the first shot's start stays put, and it is one undo step. Any camera move into or out of that cut speeds up or slows down to fit.
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

### Preview lag while playing

While playing, REAPER prepares video frames a few seconds ahead. When you edit a shot during playback, REAPER's Video window can take a few seconds to show the change. Video view in RAV is always up to date, and renders are always exact.

While playing with REAPER's Video window open, the line above the Video view frame shows that delay, for example `REAPER preview +2.4 s` (hover it for details). Rendering has no offset: every frame is drawn for its own time. Turn it off with **REAPER preview lag** in the viewer menu's Performance section.

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
