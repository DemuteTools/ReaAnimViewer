// SPDX-License-Identifier: MIT
//
// Placement plan (13-6, pure): where and how a placed item lands.
// Epic 13 (D20). Created by 13-2 with declared interfaces; story 13-6 filled it.
// Shared types: sound_model.h.

#pragma once

#include <functional>
#include <string>

#include "sound_model.h"

namespace rav {

// What the folder-item rule reads of an item (spike 13-0 C: an NVK folder item has an EMPTY
// source and P_EXT nvk_item_type "folder"). 13-6 E8: a folder item sits on a folder track
// (I_FOLDERDEPTH 1), has an empty source (no take, null source, type "" / EMPTY) and either
// child items inside its span or the NVK mark.
struct FolderItemFacts {
    bool        on_folder_track = false;
    bool        empty_source = false;
    bool        children_inside = false;  // a child track holds an item inside its span (+-1 ms)
    std::string nvk_item_type;            // the item's nvk_item_type ("" = none)
};
// True for a folder item: never copied, never tagged.
bool IsFolderItemRule(const FolderItemFacts& facts);
// The value of nvk_item_type in the item-level <EXTI block of an item state chunk ("" = none;
// a take's <EXT block is not read).
std::string NvkItemTypeFromChunk(const std::string& chunk);
// The chunk of a copy: IGUID = item_guid, every GUID / FXID line gets new_guid(), SEL 0; every
// other line byte-identical.
std::string PrepareCopyChunk(const std::string& chunk, const std::string& item_guid,
                             const std::function<std::string()>& new_guid);
// True when [inner_pos, inner_pos+inner_len] lies inside [outer_pos, outer_pos+outer_len] +- tol.
bool SpanInside(double outer_pos, double outer_len, double inner_pos, double inner_len, double tol = 0.001);
// A pile member's position: the anchor plus its offset from the source anchor item.
double PileMemberPosition(double anchor, double src_anchor_pos, double member_src_pos);

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
// Blend offset (ms) and the user's nudge (s); clamped at 0.
double PlacementPosition(double event_t, double sync_offset, double blend_ms, double hand_offset_s);
// The item volume (D_VOL, linear) for a gain in dB (NaN -> 1).
double ItemGainFor(double gain_db);

}  // namespace rav
