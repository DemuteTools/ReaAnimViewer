// SPDX-License-Identifier: MIT
//
// See ravlib.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-11.

#include "ravlib.h"


namespace rav {

bool ParseRavlib(const std::string&, SoundStore*)
{
    return false;  // STUB (13-11)
}

std::string SerializeRavlib(const SoundStore&)
{
    return {};  // STUB (13-11)
}

}  // namespace rav
