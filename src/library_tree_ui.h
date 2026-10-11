// SPDX-License-Identifier: MIT
//
// Library view (13-7a): the Libraries and Switch Groups lists (column 1) and the library tree (column 2).
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-7a
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32


namespace rav {

void DrawLibrariesSection();
void DrawSwitchGroupsSection();
void DrawLibraryTree();

}  // namespace rav

#endif  // _WIN32
