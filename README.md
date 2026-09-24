# DM ReaAnimViewer

3D animation viewer for REAPER. Load glTF and FBX animations straight onto your timeline and watch the animated character play in sync with the REAPER playhead, from any camera angle.

<!-- TODO: add a hero screenshot / GIF of the viewer docked next to the arrange view -->

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
- **Everything bundled:** No ReaImGui, no SWS, no runtime to install. ReaPack installs one file and that's it.

---

## Table of Contents

- [Installation](#installation)
  - [Install with the Demute Reaper Toolkit (easiest)](#install-with-the-demute-reaper-toolkit-easiest)
  - [Install with ReaPack](#install-with-reapack)
  - [Updating](#updating)
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
4. Select the **ReaAnimViewer** card and install it:
   - **Direct Install** downloads the extension straight into your REAPER resource folder.
   - **ReaPack Install** opens ReaPack and copies the package link to your clipboard, if you prefer to manage it through ReaPack.
5. **Restart REAPER.** Extensions are only loaded at startup.
6. Check the installation: go to **Actions > Show action list**, search for **RAV**. You should see **RAV: Open Viewer**.

### Install with ReaPack

1. Install [ReaPack](https://reapack.com/) if you don't have it already, then restart REAPER.
2. In REAPER, go to **Extensions > ReaPack > Import repositories...**
3. Paste the following URL and click **OK**:

   ```
   https://github.com/DemuteStudio/ReaAnimViewer/raw/main/index.xml
   ```

4. Go to **Extensions > ReaPack > Browse packages...**, search for **ReaAnimViewer**.
5. Right-click the package, choose **Install**, then click **Apply**.
6. **Restart REAPER.** Extensions are only loaded at startup.
7. Check the installation: go to **Actions > Show action list**, search for **RAV**. You should see **RAV: Open Viewer**.

### Updating

**With the Demute Reaper Toolkit:** open the Toolkit (**Actions > Show action list**, search for **DM_ReaperToolkit**). It automatically scans for updates when it opens. Select the **ReaAnimViewer** card, install the new version, then **restart REAPER** so it is loaded.

**With ReaPack:** go to **Extensions > ReaPack > Synchronize packages** (or wait for the automatic check), click **Apply**, then **restart REAPER** so the new version is loaded.

### Uninstalling

The Demute Reaper Toolkit cannot uninstall tools. Depending on how you installed ReaAnimViewer:

- **With ReaPack (or the Toolkit's ReaPack Install):** go to **Extensions > ReaPack > Browse packages...**, right-click **ReaAnimViewer**, choose **Uninstall**, click **Apply** and restart REAPER.
- **With the Toolkit's Direct Install, or manually:** close REAPER, then delete `reaper_animviewer.dll` from the `UserPlugins` folder of your REAPER resource path (**Options > Show REAPER resource path in explorer/finder**, usually `%APPDATA%\REAPER\UserPlugins\`).

### Manual Installation

If you can't use ReaPack:

1. Download `reaper_animviewer.dll` from the [latest release](https://github.com/DemuteStudio/ReaAnimViewer/releases/latest).
2. **Close REAPER.**
3. In REAPER, **Options > Show REAPER resource path in explorer/finder** shows you the right folder. Copy the DLL into its `UserPlugins` subfolder (usually `%APPDATA%\REAPER\UserPlugins\`).
4. Start REAPER and look for **RAV: Open Viewer** in the action list.

### Troubleshooting

| Problem | Fix |
|---------|-----|
| **RAV: Open Viewer** does not appear in the action list | Restart REAPER after installing. Make sure you run the 64-bit Windows version of REAPER. |
| The viewer opens but stays black / shows an error | Update your graphics driver: the viewer needs OpenGL 3.3. |
| ReaPack says the file is in use when updating | Close REAPER, reopen it, and run **Synchronize packages** again before loading a project. |

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
git clone https://github.com/DemuteStudio/ReaAnimViewer.git
cd ReaAnimViewer
build.bat
```

`build.bat` compiles the extension and installs it into REAPER (close REAPER first). Output: `build/Release/reaper_animviewer.dll`. Third-party dependencies are either vendored in `extern/` or fetched and statically linked by CMake (see [`extern/VENDORED.md`](extern/VENDORED.md)).

Build options, releases and the ReaPack workflow are documented in [`MAINTAINING.md`](MAINTAINING.md).

---

## License

MIT, see [`LICENSE.md`](LICENSE.md).

Released free for the game audio community by [Demute Studio](https://www.demute.studio/).
