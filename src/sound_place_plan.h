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
// Its inverse: the pile's anchor from where one member of it sits (13-8: a folder pile's nudge).
double PileAnchorOf(double member_pos, double src_anchor_pos, double member_src_pos);
// Where a placed item's pile is anchored, from its tag and its own position and snap (13-8):
// anchor = pos - tag.ao, sync = tag.sy; a tag without them: the item's own position and snap.
void PlacedAnchorOf(const PlacedTag& tag, double item_pos, double item_snap, double* anchor_pos, double* sync);

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

// 13-10 (Antho 2026-10-12): why the engine refuses to move (`edit` false) or replace / remove
// (`edit` true) a placed item; "" = allowed. An attached sound is never replaced or removed.
std::string PlacedItemRefusal(bool found, bool has_tag, bool folder, Ownership own, bool edit);

// 13-11 (B9): a User Library File Sound is imported muted and marked a library source
// (kLibSourceTagKey) so it never plays past the content. A copy of a library source is placed
// unmuted; any other source's mute state is copied as is.
bool PlacedCopyMuted(bool src_muted, bool src_is_library_source);
// 13-11 (B9): where a newly imported library source lands on its layer track: right after the last
// library source already on that track (+1 s), else 1 s after the project end.
double LibrarySourcePosition(double project_end, bool track_has_library_source, double last_library_source_end);

}  // namespace rav
