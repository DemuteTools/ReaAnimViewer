// SPDX-License-Identifier: MIT
//
// Story 13-26 -- "Clear item data" (REAPER side): the Tagging view's item and every other member
// of its pool (10-6 linked copies) go back to freshly imported, in ONE undo point "RAV: Clear item
// data". The decisions are item_clear_plan.h's (host-tested):
//
//   - the markers RAV owns on each item (committed and previews) are deleted first;
//   - each item's P_EXT:RAV_RULES (made unique: the header and its pool line stay),
//     P_EXT:RAV_RULES_COMMITTED and P_EXT:RAV_SOUNDS go;
//   - every project item tagged (P_EXT:RAV_SND) with one of them as owner, or listed in their sound
//     records, is deleted from the timeline, attached ones included (RemovePlacedAny); a Project
//     Library Sound's source item is only untagged and stays.
//
// Never the animation items themselves, other items' records, the sound store, RAV tracks, untagged
// items or markers RAV does not own. Not the REAPER selection: the item given and its pool.
//
// Main thread only. No-throw.

#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

#include "item_clear_plan.h"
#include "reaper_api.h"

namespace rav {

// What a clear of `item` (and its linked copies) would remove, read now (nothing written).
struct ItemClearInfo {
    bool            anything = false;  // false: nothing to clear (or no RAV item)
    ItemClearCounts counts;
};
ItemClearInfo ReadItemClear(MediaItem* item);

struct ItemClearResult {
    bool                    ran = false;  // something was written (one undo point)
    ItemClearCounts         counts;
    std::vector<MediaItem*> items;        // the items cleared (the item, then its copies)
    std::string             error;        // non-empty: a record could not be written (the clear stopped)
};
// The gesture (one undo point, "RAV: Clear item data").
ItemClearResult ClearItemData(MediaItem* item);

}  // namespace rav

#endif  // _WIN32
