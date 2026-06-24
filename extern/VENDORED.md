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
| Pinned commit | `31234f3323227c3342133d665cda6fa0cc5e9ae9` |
| Commit date | 2026-05-07 |
| Commit message | `update sdk for 7.72` |
| Reaper version compat | 7.72+ |

**Vendored files (extracted from upstream `sdk/`):**

- `sdk/reaper_plugin.h` — plugin entry-point macros, `reaper_plugin_info_t` struct, type aliases
- `sdk/reaper_plugin_functions.h` — function-pointer typedefs for the Reaper API surface (loaded at runtime via `rec->GetFunc`)
- `sdk/LICENSE` — upstream license text

**Files intentionally not vendored:** `reaper_plugin_fx_embed.h`, `reaper_vst3_interfaces.h`, `video_frame.h`, `video_processor.h`, `localize-import.h`, `example_*` directories — not needed for Phase 0 (no embedded FX, no VST3, no video pipeline). Add them to this directory if a later phase requires them.

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
cp /tmp/reaper-sdk/sdk/{reaper_plugin.h,reaper_plugin_functions.h,LICENSE} extern/reaper-sdk/sdk/
git -C /tmp/reaper-sdk rev-parse HEAD  # update Pinned commit in this file
rm -rf /tmp/reaper-sdk
```

Update the table above and commit with message `chore(extern): bump reaper-sdk to <short-sha>`.

## extern/WDL (deferred → likely no longer needed)

WDL/SWELL was originally part of the project's stack (per the PRFAQ) for a future Mac/Linux port. The 2026-05-10 architectural decision to render the viewer inside a ReaImGui dockable panel removes the cross-platform window-management burden from us — ReaImGui already uses SWELL internally on Mac/Linux. WDL vendoring is therefore likely never needed; we re-evaluate at the Mac/Linux port phase, but the default position is to not vendor it.

## extern/reaimgui (runtime dependency, header-only vendoring)

| Field | Value |
|---|---|
| Upstream | https://github.com/cfillion/reaimgui |
| Distribution | ReaPack: `cfillion/reaimgui` (auto-installed as our extension's dependency) |
| License | Per-source, mixed (LGPL3+ for the extension; MIT for the underlying Dear ImGui). See upstream LICENSE. |

**Runtime model:** ReaImGui ships as a separate Reaper extension (DLL). Our extension calls its API via `rec->GetFunc("ImGui_*")` function-pointer resolution — same pattern as the Reaper API itself. At plugin load we verify that ReaImGui is present (e.g. `ImGui_CreateContext` resolves non-null); if not, we emit a console message instructing the user to install `cfillion/reaimgui` and bail cleanly without registering our action.

**Header vendoring:** the ReaImGui project ships a generated `reaper_imgui_functions.h` header analogous to `reaper_plugin_functions.h`. We vendor that header under `extern/reaimgui/include/` to get function-pointer typedefs and the `IMGUI_*` enum/struct definitions we use. Pinned commit recorded here at the time of vendoring (TBD when Phase 0.5 lands).

**Why not statically link Dear ImGui directly?** Three reasons:
1. ReaImGui already solves Reaper-specific concerns (docking integration with Reaper's docker, theming, project state hooks). Re-doing that from scratch would be weeks of work.
2. Visual consistency with the rest of the Reaper ecosystem — users see ReaImGui's familiar look across many ReaPack tools.
3. Updates to ImGui (security/UX) propagate via ReaPack without us shipping a new DLL.

**Re-vendoring procedure** (when ReaImGui upgrades its API surface):

```sh
git clone --depth 1 https://github.com/cfillion/reaimgui.git /tmp/reaimgui
cp /tmp/reaimgui/api/reaper_imgui_functions.h extern/reaimgui/include/
git -C /tmp/reaimgui rev-parse HEAD  # record pinned commit in this file
rm -rf /tmp/reaimgui
```
