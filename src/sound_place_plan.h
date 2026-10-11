// SPDX-License-Identifier: MIT
//
// Placement plan (13-6, pure): where and how a placed item lands.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-6
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#include <string>

#include "sound_model.h"

namespace rav {

// What the folder-item rule reads of an item (spike 13-0 C: an NVK folder item has an EMPTY
// source and P_EXT nvk_item_type "folder").
struct FolderItemFacts {
    bool        empty_source = false;
    std::string nvk_item_type;  // the item's P_EXT:nvk_item_type ("" = none)
};
// True for a folder item: never copied, never tagged.
bool IsFolderItemRule(const FolderItemFacts& facts);

// How a slice variation is applied to the placed item: take D_STARTOFFS, item D_LENGTH,
// D_SNAPOFFSET 0. `set` false = leave the item as copied (a Take variation sets I_CURTAKE).
struct SliceApply {
    bool   set = false;
    double start_offs = 0.0;
    double length = 0.0;
    double snap_offset = 0.0;
};
SliceApply SliceApplyFor(const SoundFrame& frame);

// The placed item's position (project s): the event, minus the entry's sync offset, plus the
// Blend offset (ms) and the user's nudge (s).
double PlacementPosition(double event_t, double sync_offset, double blend_ms, double hand_offset_s);
// The item volume (D_VOL, linear) for a gain in dB.
double ItemGainFor(double gain_db);

}  // namespace rav
