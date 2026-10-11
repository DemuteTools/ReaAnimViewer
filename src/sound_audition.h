// SPDX-License-Identifier: MIT
//
// Sound audition (13-7b, REAPER): plays a node of a library.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-7b
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32


namespace rav {

void InitSoundAudition(void* (*get_func)(const char* name));
// Stops any audition playing.
void StopAudition();

}  // namespace rav

#endif  // _WIN32
