// SPDX-License-Identifier: MIT
//
// See sound_populate.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-8.

#include "sound_populate.h"

#ifdef _WIN32


namespace rav {

SoundReport PopulateItems(const std::vector<MediaItem*>&)
{
    return {};  // STUB (13-8)
}

}  // namespace rav

#endif  // _WIN32
