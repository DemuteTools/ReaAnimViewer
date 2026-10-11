// SPDX-License-Identifier: MIT
//
// See sound_draw.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-3.

#include "sound_draw.h"


namespace rav {

int DrawPick(const std::vector<int>&, int, const std::vector<int>&, uint64_t, uint64_t)
{
    return -1;  // STUB (13-3)
}

std::vector<int> ActiveVariations(const SoundData&)
{
    return {};  // STUB (13-3)
}

uint64_t EventSalt(const std::string&, double)
{
    return 0;  // STUB (13-3)
}

std::vector<const SoundNode*> BlendParts(const SoundLibrary&, const SoundNode&)
{
    return {};  // STUB (13-3)
}

}  // namespace rav
