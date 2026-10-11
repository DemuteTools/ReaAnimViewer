// SPDX-License-Identifier: MIT
//
// The Tagging footer's Attach button (13-10).
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-10
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32

#include "tagging_sound_context.h"

namespace rav {

// The Attach button's width (px), 0 when it does not show.
float AttachButtonWidth();
// Attach, on the Populate row (after DrawPopulateRow).
void DrawAttachButton(const TaggingSoundContext& ctx);

}  // namespace rav

#endif  // _WIN32
