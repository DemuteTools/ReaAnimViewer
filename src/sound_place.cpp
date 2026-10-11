// SPDX-License-Identifier: MIT
//
// See sound_place.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-6.

#include "sound_place.h"

#ifdef _WIN32


namespace rav {

void InitSoundPlace(void* (*)(const char*))
{
    // STUB (13-6)
}

bool ReadItemFacts(MediaItem*, ItemFacts* out)
{
    if (out) *out = ItemFacts{};
    return false;  // STUB (13-6)
}

bool IsFolderItem(MediaItem*)
{
    return false;  // STUB (13-6)
}

PlaceResult PlaceSounds(ReaProject*, const PlaceRequest&)
{
    return {};  // STUB (13-6)
}

bool MovePlacement(ReaProject*, const std::string&, double)
{
    return false;  // STUB (13-6)
}

PlaceResult ReplacePlacement(ReaProject*, const std::string&, const PlaceRequest&)
{
    return {};  // STUB (13-6)
}

bool RemovePlacement(ReaProject*, const std::string&)
{
    return false;  // STUB (13-6)
}

bool ReadCopyImportedMedia()
{
    return false;  // STUB (13-6)
}

}  // namespace rav

#endif  // _WIN32
