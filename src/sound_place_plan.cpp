// SPDX-License-Identifier: MIT
//
// See sound_place_plan.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-6.

#include "sound_place_plan.h"


namespace rav {

bool IsFolderItemRule(const FolderItemFacts&)
{
    return false;  // STUB (13-6)
}

SliceApply SliceApplyFor(const SoundFrame&)
{
    return {};  // STUB (13-6)
}

double PlacementPosition(double, double, double, double)
{
    return 0.0;  // STUB (13-6)
}

double ItemGainFor(double)
{
    return 1.0;  // STUB (13-6)
}

}  // namespace rav
