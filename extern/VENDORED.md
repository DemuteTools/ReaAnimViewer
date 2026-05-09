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

## Re-vendoring procedure

To bump the SDK version:

```sh
git clone --depth 1 https://github.com/justinfrankel/reaper-sdk.git /tmp/reaper-sdk
cp /tmp/reaper-sdk/sdk/{reaper_plugin.h,reaper_plugin_functions.h,LICENSE} extern/reaper-sdk/sdk/
git -C /tmp/reaper-sdk rev-parse HEAD  # update Pinned commit in this file
rm -rf /tmp/reaper-sdk
```

Update the table above and commit with message `chore(extern): bump reaper-sdk to <short-sha>`.

## extern/WDL (deferred)

WDL/SWELL is part of the project's stack (per the PRFAQ), but Phase 0 uses
direct Win32 + WGL on Windows and does not need SWELL. WDL will be
vendored when the Linux/macOS port is implemented (Phase 1+). At that
point a `extern/WDL/` entry will be added here following the same
pattern.
