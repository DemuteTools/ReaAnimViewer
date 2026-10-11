// SPDX-License-Identifier: MIT
//
// See sound_audition.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-7b.

#include "sound_audition.h"

#ifdef _WIN32


namespace rav {

void InitSoundAudition(void* (*)(const char*))
{
    // STUB (13-7b)
}

void StopAudition()
{
    // STUB (13-7b)
}

}  // namespace rav

#endif  // _WIN32
