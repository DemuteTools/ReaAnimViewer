// SPDX-License-Identifier: MIT
//
// See sound_switch.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-4.

#include "sound_switch.h"


namespace rav {

Resolution ResolveEvent(const SoundStore&, const SoundLibrary&, const ValueLayers&, const ResolveHooks&)
{
    return {};  // STUB (13-4)
}

std::string EffectiveValue(const SoundNode&, const ValueLayers&, ValueFrom* from)
{
    if (from) *from = ValueFrom::Default;
    return {};  // STUB (13-4)
}

LibraryCheck CheckLibrary(const SoundLibrary&)
{
    return {};  // STUB (13-4)
}

bool ConvertNodeType(SoundLibrary&, const std::string&, SoundNodeType, const std::string&, const SoundStore&)
{
    return false;  // STUB (13-4)
}

std::string SwitchNameKey(const std::string&)
{
    return {};  // STUB (13-4)
}

}  // namespace rav
