# Vendored third-party sources

This directory holds copies of upstream sources we depend on, committed
directly to the tree (no git submodules).

**Why vendored, not submodule:** the project repository lives on a Windows
drive accessed from WSL via the 9p `drvfs` protocol, which forbids the
`chmod` calls `git submodule add` performs internally. Vendoring also
removes one step from the developer workflow — fresh clones are usable
immediately, no `git submodule update --init` required.

## extern/reaper-sdk/

| Field | Value |
|---|---|
| Upstream | https://github.com/justinfrankel/reaper-sdk |
| License | zlib (see `extern/reaper-sdk/sdk/LICENSE`) |
| Pinned commit | `490ded57668727fba21482fabc50ba9853a457bb` |
| Archive SHA256 | `2ce09e8a40e8827303f5f1801a646013f9ad798032ac5f2515bce23d66439b48` (`archive/490ded5….zip`) |
| Bumped by | Story 11-1 (2026-10-03), from `31234f3` — the commit spike 11-0 proved in REAPER 7.79 |
| Reaper version compat | 7.72+ (video processor API: 6.x+) |

**Vendored files (extracted from upstream `sdk/`):**

- `sdk/reaper_plugin.h` — plugin entry-point macros, `reaper_plugin_info_t` struct, type aliases; also documents `cockos.reaper_extension` + `clap_get_reaper_context` for CLAP plug-ins
- `sdk/reaper_plugin_functions.h` — function-pointer typedefs for the Reaper API surface (loaded at runtime via `rec->GetFunc`)
- `sdk/video_processor.h`, `sdk/video_frame.h` — `IREAPERVideoProcessor` / `IVideoFrame`, used by the video FX `rav_video_fx.clap` (Epic 11)
- `sdk/LICENSE` — upstream license text

**Files intentionally not vendored:** `reaper_plugin_fx_embed.h`, `reaper_vst3_interfaces.h` (the video FX is a CLAP, not a VST3: spike 11-0 amendment 7), `localize-import.h`, `example_*` directories.

## extern/clap/ (CLAP 1.2.7 headers — the video FX)

| Field | Value |
|---|---|
| Upstream | https://github.com/free-audio/clap |
| License | MIT (see `extern/clap/LICENSE`) |
| Pinned tag | `1.2.7` |
| Archive SHA256 | `7fe908e6c4244e791db43a6106938ceda96081ea4dc5f739755e68edb9e2ed42` (`archive/refs/tags/1.2.7.zip`) |
| Delivery | In-tree copy of upstream `include/` (header-only C API, ~70 small files) + `LICENSE` |

Used only by `rav_video_fx.clap` (`src/fx/rav_video_fx_clap.cpp`), included as a **SYSTEM** include so it cannot trip `/W3 /permissive-`. Header-only: no library, no runtime dependency. Introduced by Story 11-1 (Epic 11, CLAP chosen over VST3 by spike 11-0).

## GLM 1.0.3 (matrix math — FetchContent pin, not in-tree)

| Field | Value |
|---|---|
| Upstream | https://github.com/g-truc/glm |
| License | MIT (and Happy Bunny) |
| Pinned tag | `1.0.3` |
| Delivery | CMake `FetchContent` (see root `CMakeLists.txt`), **not** committed to the tree |

**Why FetchContent and not in-tree vendoring:** AR6's default is to vendor header
trees, but GLM is a large multi-hundred-file header tree and copying it onto the
drvfs/WSL mount by hand is impractical (the same `chmod`/path friction that drove
the no-submodule policy). The story sanctions FetchContent pinned to the `1.0.3`
tag as the fallback, which is also the approach Spike 0 proved. Header-only and
column-major right-handed by default (matches D3). Included as a **SYSTEM** include
so its headers cannot trip `/W3 /permissive-` (NFR-R5).

Introduced by Story 1.2 (the renderer is the project's first matrix consumer).

## stb_image (image decoder — FetchContent pin, not in-tree)

| Field | Value |
|---|---|
| Upstream | https://github.com/nothings/stb |
| License | Public domain (Unlicense) / MIT — dual-licensed, see header footer |
| Pinned commit | `31c1ad37456438565541f4919958214b6e762fb4` |
| Delivery | CMake `FetchContent` (see root `CMakeLists.txt`), **not** committed to the tree |

**Why a decoder at all:** assimp does **not** decode embedded compressed textures —
its glTF importer leaves an embedded PNG/JPG as raw file bytes in `aiTexture`
(`mHeight == 0`, bytes in `pcData`) and exposes no public decode API. `stb_image` is
the de-facto standard single-header public-domain decoder (and what assimp itself
vendors internally, though it does not export it), so an explicit pin is the clean
choice. **Pinned by commit** because stb ships no release tags. The implementation
(`STB_IMAGE_IMPLEMENTATION`) is compiled into **`asset_loader.cpp` only** (the one TU
already allowed heavy third-party headers); the format set is narrowed to
PNG/JPEG/TGA/BMP (`STBI_ONLY_*`, mirroring the AR5 importer narrowing). Header-only —
no new link library, single-DLL invariant (D15/D17) unaffected. Included as a
**SYSTEM** include so it cannot trip `/W3 /permissive-` (NFR-R5).

Introduced by Story 2.2 (first texture decode/upload).

## Re-vendoring procedure

To bump the SDK version:

```sh
git clone --depth 1 https://github.com/justinfrankel/reaper-sdk.git /tmp/reaper-sdk
cp /tmp/reaper-sdk/sdk/{reaper_plugin.h,reaper_plugin_functions.h,video_processor.h,video_frame.h,LICENSE} extern/reaper-sdk/sdk/
git -C /tmp/reaper-sdk rev-parse HEAD  # update Pinned commit in this file
rm -rf /tmp/reaper-sdk
```

To bump CLAP: download `https://github.com/free-audio/clap/archive/refs/tags/<tag>.zip`, replace
`extern/clap/include/` and `extern/clap/LICENSE` with the archive's, update the table above. A new
CLAP version must stay ABI-compatible (`clap_version_is_compatible`); REAPER checks it.

Update the table above and commit with message `chore(extern): bump reaper-sdk to <short-sha>`.

## extern/WDL (deferred → likely no longer needed)

WDL/SWELL was originally part of the project's stack (per the PRFAQ) for a future Mac/Linux port. The 2026-05-10 architectural decision to render the viewer inside a ReaImGui dockable panel removes the cross-platform window-management burden from us — ReaImGui already uses SWELL internally on Mac/Linux. WDL vendoring is therefore likely never needed; we re-evaluate at the Mac/Linux port phase, but the default position is to not vendor it.

## Dear ImGui 1.91.5 (in-viewport tool UI — FetchContent pin, not in-tree)

| Field | Value |
|---|---|
| Upstream | https://github.com/ocornut/imgui |
| License | MIT (see upstream `LICENSE.txt`) |
| Pinned tag | `v1.91.5` |
| Delivery | CMake `FetchContent` (see root `CMakeLists.txt`), **not** committed to the tree (same `drvfs` reason as GLM/assimp/stb) |
| Compiled units | `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `backends/imgui_impl_win32.cpp`, `backends/imgui_impl_opengl3.cpp` |

**Decision (Story 6.5.3 review revision, 2026-06-28):** we **statically vendor Dear ImGui INTO the plugin** and render it into our own GL context (true in-viewport overlay), rather than depending on the **ReaImGui** extension. This **supersedes** the earlier plan (in prior revisions of this file) to depend on ReaImGui at runtime. The `## extern/WDL` note above also referenced that superseded ReaImGui-panel plan — the viewer is a native GL window (Spike-0), and Dear ImGui's Win32+OpenGL3 backends drop onto the context + `WndProc` we already own.

**Why static Dear ImGui, not the ReaImGui runtime dependency:**
1. **Self-contained** — no ReaPack install step for users; our single DLL just works (the whole value of the tool is being drop-in). ReaImGui would force every user to install `cfillion/reaimgui`.
2. **True overlay** — ReaImGui renders into its own window and cannot paint over our native GL viewport; it would be a *separate* control panel. Vendored Dear ImGui draws directly in our 3D frame (no flicker, controls sit on the viewport).
3. **Already on a native GL context** — the OpenGL3 + Win32 backends need exactly what we have.

Built as a **separate static lib** (`add_library(imgui …)`) so our strict `/W3 /permissive-` flags do not apply to upstream code; the extension includes its headers `SYSTEM`. XInput is loaded dynamically by the Win32 backend (no link-time xinput dependency). Single-DLL invariant (D15/D17) unaffected — ImGui is statically linked into `reaper_animviewer.dll`.

**Re-pinning procedure** (when bumping the ImGui version): change `GIT_TAG` in the root `CMakeLists.txt`, update the tag above, and re-verify the backend API (`ImGui_ImplWin32_Init`, `ImGui_ImplOpenGL3_Init`, `ImGui_ImplWin32_WndProcHandler`) against the new release.
