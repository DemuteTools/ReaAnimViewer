// SPDX-License-Identifier: MIT
//
// Attach (13-10, REAPER): attaches the selected sound items to the shown animation item's events.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-10
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32

#include "reaper_api.h"
#include "sound_model.h"

namespace rav {

// Attaches the selected (non-RAV) items to the nearest events of `anim_item`: one undo point.
SoundReport AttachSelectedTo(MediaItem* anim_item);

}  // namespace rav

#endif  // _WIN32
