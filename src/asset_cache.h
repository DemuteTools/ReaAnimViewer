// SPDX-License-Identifier: MIT
//
// Parsed models shared between threads (Story 11-2). The viewer (UI thread) and the
// video FX (REAPER's video thread) each draw with their OWN GPU copy, made from the same
// immutable CPU parse (asset_loader.h CpuAsset), held here by file path.
//
//  - A model is only ever handed out complete (std::shared_ptr<const CpuAsset>): a thread
//    that holds one keeps it alive even if the cache drops it, so a reload or an item
//    removal while playing can never show a half-built model.
//  - A failed load is remembered (no re-parse every frame) until the file changes.
//  - The main thread keeps the files the video FX tracks use and (freeze fix #1) the most
//    recently used other parses, bounded by count and size (asset_lru.h), and drops a file
//    whose date or size changed on disk, so a re-export is read again.
// All functions are thread-safe and never throw.

#pragma once

#ifdef _WIN32

#include <memory>
#include <string>
#include <vector>

#include "asset_loader.h"

namespace rav {

// The parse of `path`. On a miss it is loaded on the CALLING thread (blocking, GL-free).
// Null when the file cannot be loaded (logged once per failure).
std::shared_ptr<const CpuAsset> AcquireCpuAsset(const std::string& path);

// One version of a file on disk (its date and size). Read it BEFORE parsing the file, so a
// re-export during the parse is seen as a change by the 1 Hz check. Zero when unreadable.
struct AssetFileStamp {
    long long          time = 0;
    unsigned long long size = 0;
    bool operator==(const AssetFileStamp& o) const { return time == o.time && size == o.size; }
    bool operator!=(const AssetFileStamp& o) const { return !(*this == o); }
};
AssetFileStamp ReadAssetFileStamp(const std::string& path);

// AcquireCpuAsset, and the file version the parse handed out was read from (`*stamp_out`; zero
// when none): freeze fix #1, the follow keeps a detection only for the version it ran on.
std::shared_ptr<const CpuAsset> AcquireCpuAsset(const std::string& path, AssetFileStamp* stamp_out);

// Freeze fix #2: the cold paths' lookup (the viewer's item change, the Tagging view's file): the
// parse of `path` for the file's version `now` (ReadAssetFileStamp just read): an entry of another
// version is parsed again. The viewer and the Tagging view share it (one parse per file version,
// also the video FX's: an unchanged file is never re-uploaded by the video thread). On a failure,
// its category, detail and hint (kept with the entry until the file changes). `used` (optional):
// the version handed out (read before its parse).
CpuLoadResult AcquireCpuAssetAt(const std::string& path, const AssetFileStamp& now, AssetFileStamp* used);

// Drops the parse of `path` (if any), so the next AcquireCpuAsset reads the file again.
// For a change the file's date and size do not show, e.g. texture files copied next to
// it by "Locate textures..." (issue #1). Threads that hold the old parse keep it. A parse
// already running on another thread when this is called is not stored when it finishes
// (13-51: it may predate the change).
void EvictCpuAsset(const std::string& path);

// Main thread: `wanted` = every file the video FX tracks use now (always kept). The other
// entries are kept within the recent files' bounds; with check_files, entries whose file
// changed on disk are dropped.
void TrimCpuAssetCache(const std::vector<std::string>& wanted, bool check_files);

// Unload: drop everything.
void ClearCpuAssetCache();

}  // namespace rav

#endif  // _WIN32
