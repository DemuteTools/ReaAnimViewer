// SPDX-License-Identifier: MIT
//
// Populate (13-8, REAPER): places each event's sound for the given animation items, one undo point.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-8
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32

#include <vector>

#include "reaper_api.h"
#include "sound_model.h"

namespace rav {

// Populates the items (the selected animation items): one undo point "RAV: Populate sounds".
SoundReport PopulateItems(const std::vector<MediaItem*>& items);

}  // namespace rav

#endif  // _WIN32
