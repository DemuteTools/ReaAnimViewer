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
//  - The main thread trims the cache to the files the video FX tracks use, and drops a
//    file whose date or size changed on disk, so a re-export is read again.
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

// The viewer's own parse of `path`, kept only if a video FX track uses that file (the
// cache does not hold models nobody renders as video). `stamp` = ReadAssetFileStamp taken
// before that parse. An entry already holding that same version is kept as it is, so the
// video thread does not re-upload an unchanged model each time the viewer switches items.
void OfferCpuAsset(const std::string& path, std::shared_ptr<const CpuAsset> asset,
                   const AssetFileStamp& stamp);

// Drops the parse of `path` (if any), so the next AcquireCpuAsset reads the file again.
// For a change the file's date and size do not show, e.g. texture files copied next to
// it by "Locate textures..." (issue #1). Threads that hold the old parse keep it.
void EvictCpuAsset(const std::string& path);

// Main thread: `wanted` = every file the video FX tracks use now. Other entries are
// dropped; with check_files, entries whose file changed on disk are dropped too.
void TrimCpuAssetCache(const std::vector<std::string>& wanted, bool check_files);

// Unload: drop everything.
void ClearCpuAssetCache();

}  // namespace rav

#endif  // _WIN32
