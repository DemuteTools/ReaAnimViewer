// SPDX-License-Identifier: MIT
//
// Story 13-26 -- "Clear item data", planned (pure). The Tagging view's item and its linked copies
// (10-6 pool) go back to freshly imported: their rules records, Cancel snapshots and sound records
// go, the markers RAV owns on them go, and every sound RAV placed or attached for them is deleted
// from the timeline (a Project Library Sound's source item is only untagged: it stays).
//
//   - A rules record with a `pool id=` line (made unique) is rewritten as its header and that pool
//     line only, so the item stays unlinked. Any other record (and one that does not read) goes.
//   - A sound item goes when its RAV_SND tag names a target as owner, or when a target's sound
//     record lists its GUID among its placements (and it carries a tag: an untagged item is the
//     user's, never touched).
//
// No REAPER: host-tested (tests/item_clear_plan_test.cpp). The gesture: item_clear.h.

#pragma once

#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

// One item cleared (the shown item first, then its linked copies), as read.
struct ClearTarget {
    std::string guid;           // the item's GUID
    std::string rules_raw;      // its take's P_EXT:RAV_RULES ("" = none)
    std::string committed_raw;  // P_EXT:RAV_RULES_COMMITTED ("" = none)
    std::string sounds_raw;     // P_EXT:RAV_SOUNDS ("" = none)
};

// One project item that carries a P_EXT:RAV_SND tag (its text as read).
struct ClearTagged {
    std::string guid;
    std::string tag_raw;
};

// What one target's records become.
struct ClearRecordWrite {
    size_t      target = 0;           // index into the targets
    bool        rules = false;        // the rules key is written:
    std::string rules_text;           //   ... with this text ("" = the key removed)
    bool        committed = false;    // the Cancel snapshot key is removed
    bool        sounds = false;       // the sound record key is removed
};

struct ItemClearCounts {
    int items = 0;     // the targets (the item + its linked copies)
    int copies = 0;    // the linked copies (items - 1, never below 0)
    int events = 0;    // detected + user events in the records
    int rules = 0;     // rules (blocks, auto ones included) in the records
    int markers = 0;   // the markers RAV owns on them (committed + previews; hidden ones not counted)
    int sounds = 0;    // the sound items deleted
    int attached = 0;  // ... of which attached by the user
};

struct ItemClearPlan {
    std::vector<std::string>      delete_guids;  // sound items deleted
    std::vector<std::string>      untag_guids;   // library source items: tag cleared, kept
    std::vector<ClearRecordWrite> records;       // targets whose keys change
    std::vector<bool>             markers;       // per target: its record reads and lists markers to delete
    ItemClearCounts               counts;
    bool                          anything = false;  // false: nothing to clear (the button is off)
};

// GUIDs are compared ignoring ASCII case. `library_source_guids`: every Sound source item of the
// project's libraries (SoundSource::item_guid).
ItemClearPlan PlanItemClear(const std::vector<ClearTarget>& targets, const std::vector<ClearTagged>& tagged,
                            const std::vector<std::string>& library_source_guids);

// Every item a library Sound comes from (SoundSource::item_guid, whatever the source kind: a Sound
// converted to a User Library keeps its project item), across all the store's libraries.
std::vector<std::string> LibrarySourceGuids(const SoundStore& store);

// The record a made-unique item keeps: its header and its pool line ("" when `raw` does not read
// or has no pool line).
std::string PoolOnlyRecord(const std::string& raw);

// The confirm band: "Clear this item and its 2 linked copies?" / "Clear this item?".
std::string ClearConfirmTitle(int copies);
// "42 events · 12 rules · 84 markers · 37 sounds (3 attached by you) are removed. One undo step."
// (zero parts left out; none left: "Its settings are removed. One undo step.").
std::string ClearConfirmText(const ItemClearCounts& c);
// After the gesture: "Cleared: 1 item + 2 copies · 37 sounds removed".
std::string ClearDoneText(const ItemClearCounts& c);

}  // namespace rav
