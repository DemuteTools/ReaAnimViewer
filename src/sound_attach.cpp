// SPDX-License-Identifier: MIT
//
// See sound_attach.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-10.

#include "sound_attach.h"

#ifdef _WIN32


namespace rav {

SoundReport AttachSelectedTo(MediaItem*)
{
    return {};  // STUB (13-10)
}

}  // namespace rav

#endif  // _WIN32
