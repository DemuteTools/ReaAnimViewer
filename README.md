<img width="2116" height="957" alt="image" src="https://github.com/user-attachments/assets/46506343-093d-42c8-ba8e-0e8efb70d41e" />

# DM ReaAnimViewer

3D animation viewer for REAPER. Load glTF and FBX animations straight onto your timeline and watch the animated character play in sync with the REAPER playhead, from any camera angle.

## Why Use This Tool?

Sound designing for game characters usually means screen-recording every animation in the engine, importing the videos into REAPER, and redoing all of it every time an animator changes a few frames. Once recorded, the camera angle is frozen: if the gesture you need to time is hidden behind the character, you're back to the engine. ReaAnimViewer removes the video step entirely.

- **No more screen captures:** Drop the animation file itself on a track. The rig is rendered live, frame-accurate, with no video file, no stutter, no re-recording.
- **Move the camera while you design:** Orbit, pan and zoom around the character at any time, and snap to front/side/top views with the navigation cube. See the foot contact or the hand swing the recorded video would have hidden.
- **Behaves like native REAPER media:** Animation items can be moved, trimmed, looped, saved with the project and copied into the project folder like any audio item, so your session and your sounds stay in sync.

## How It Works

ReaAnimViewer is a native REAPER extension (a single `.dll`). It teaches REAPER to read `.glb`, `.gltf` and `.fbx` files as media: drop one on a track and it becomes an item whose length matches the animation. A dockable viewer window renders the animated mesh at the current playhead position: play, stop, scrub or loop and the character follows.

- **Engine agnostic:** Works with any animation exported to glTF or FBX from Unreal, Unity, Godot, Blender, Maya or a proprietary engine.
- **Skinned meshes and textures:** Full skeletal deformation, diffuse/normal/specular maps, multi-material meshes.
- **Several animations per project:** Put different animations on different tracks; the viewer shows the one on the topmost track under the playhead.
- **Viewer tools:** A built-in side menu for lighting, floor, shadows and render quality.
- **Everything bundled:** No ReaImGui, no SWS, no runtime to install. The whole viewer is a single extension file.

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
- [Working with Animation Items](#working-with-animation-items)
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

**Manual install:** download the new DLL and replace the old one, as in [Manual Installation](#manual-installation).

### Upgrading from 0.1.x

Version 0.1.x came as two ReaPack packages, also when you installed it through the Toolkit. Remove the old one once:

1. Go to **Extensions > ReaPack > Browse packages...** and search for **ReaAnimViewer**.
2. Two entries are listed. Right-click the one whose category is **Extensions**, choose **Uninstall**, then click **Apply**. Keep the one in **Scripts**.
3. Restart REAPER, then run **Extensions > ReaPack > Synchronize packages** and click **Apply**.
4. Restart REAPER again.

Until you do this, ReaPack reports a conflict on `reaper_animviewer.dll` and ReaAnimViewer stays on 0.1.x.

### Uninstalling

**With ReaPack:** go to **Extensions > ReaPack > Browse packages...**, right-click **ReaAnimViewer**, choose **Uninstall**, click **Apply** and restart REAPER. Don't delete the DLL by hand instead: ReaPack would still consider it installed (see [Troubleshooting](#troubleshooting)).

**With the Demute Reaper Toolkit:** uninstall the **ReaAnimViewer** card in the Toolkit, then close REAPER and delete `reaper_animviewer.dll` from the `UserPlugins` folder.

**Manual install:** close REAPER and delete `reaper_animviewer.dll` from the `UserPlugins` folder.

To find the `UserPlugins` folder: in REAPER, **Options > Show REAPER resource path in explorer/finder** (usually `%APPDATA%\REAPER\UserPlugins\`). Files named `reaper_animviewer.dll.old` or `.new` next to it are leftovers of an update and can be deleted too.

### Manual Installation

If you can't use ReaPack:

1. Download `reaper_animviewer.dll` from the [latest release](https://github.com/DemuteTools/ReaAnimViewer/releases/latest).
2. **Close REAPER.**
3. In REAPER, **Options > Show REAPER resource path in explorer/finder** shows you the right folder. Copy the DLL into its `UserPlugins` subfolder (usually `%APPDATA%\REAPER\UserPlugins\`).
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

Click the **menu icon** in the top-left corner of the viewer to show or hide the tool menu.

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

### Shadow

- **Off / Low / Mid / High**: Quality of the shadow cast on the floor. Off is the lightest for your GPU.

### Performance

| Option | Description |
|--------|-------------|
| **Normal maps** | Turn surface-detail maps on or off |
| **MSAA** | Edge smoothing: Off, 2x, 4x, 8x |
| **FPS** | Show the frame rate in the viewer |

If the viewer feels slow on a laptop, lower **MSAA** and **Shadow** first.

---

## Working with Animation Items

Animation items behave like regular REAPER media items:

- **Move** them to realign the animation with your sounds.
- **Trim** the start or end to show only part of the animation.
- **Loop** an item by extending it past its end.
- Put animations on **several tracks**: when items overlap under the playhead, the viewer shows the one on the **topmost track**.

Animation items produce no audio: you can place them on the same tracks as your sounds or on a dedicated "Anim" track.

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

`build.bat` compiles the extension and installs it into REAPER (close REAPER first). Output: `build/Release/reaper_animviewer.dll`. Third-party dependencies are either vendored in `extern/` or fetched and statically linked by CMake (see [`extern/VENDORED.md`](extern/VENDORED.md)).

Build options, releases and the ReaPack workflow are documented in [`MAINTAINING.md`](MAINTAINING.md).

---

## License

MIT, see [`LICENSE.md`](LICENSE.md).

Released free for the game audio community by [Demute Studio](https://www.demute.studio/).
