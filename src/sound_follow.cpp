// SPDX-License-Identifier: MIT
//
// See sound_follow.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-9.

#include "sound_follow.h"

#ifdef _WIN32


namespace rav {

void InitSoundFollow(void* (*)(const char*))
{
    // STUB (13-9)
}

void SoundFollowTick(ReaProject*)
{
    // STUB (13-9)
}

void DrawSoundsMenuSection()
{
    // STUB (13-9)
}

}  // namespace rav

#endif  // _WIN32
