// SPDX-License-Identifier: MIT
//
// Library view (13-11): User Libraries (.ravlib) in column 1 and the node panel.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-11
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32


namespace rav {

void DrawUserLibrarySection();
void DrawUserLibraryNodeSection();

}  // namespace rav

#endif  // _WIN32
