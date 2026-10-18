// SPDX-License-Identifier: MIT
//
// 13-35 (REAPER): what the Library knows of the placed sounds behind it -- the project's placed items scanned
// (sound_sync_plan.h BuildSyncState), read again only when the project or the sound store changed. The gesture itself
// is Populate's (sound_populate.h SyncPlacedSounds); the card, marks and dialogs are sound_sync_ui.h's.

#pragma once

#ifdef _WIN32

#include "sound_sync_plan.h"

namespace rav {

// The placed sounds of the current project behind the Library, with their animation items (cached: scanned again when
// REAPER's project state or the sound store moved, at most four times a second).
const SyncState& PlacedSyncState();
// Forgets the cache (the next call scans).
void InvalidatePlacedSyncState();

}  // namespace rav

#endif  // _WIN32
