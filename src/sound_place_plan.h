// SPDX-License-Identifier: MIT
//
// Placement plan (13-6, pure): where and how a placed item lands.
// Epic 13 (D20). Created by 13-2 with declared interfaces; story 13-6 filled it.
// Shared types: sound_model.h.

#pragma once

#include <functional>
#include <string>
#include <vector>

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

// Antho 2026-10-12: the fades a placed variation sets on its item (D_FADEINLEN / D_FADEOUTLEN in
// item seconds, C_FADEINSHAPE / C_FADEOUTSHAPE). A sliced or Take variation always sets both (its own
// or the Sound's default fade); a Whole-item Sound keeps its source item's own fades, except a side
// the variation set by hand (its shape only when the frame set a non-zero one: the length alone is
// the user's edit). Each side set also clears REAPER's auto-fade (D_FADE*LEN_AUTO = -1, which else
// overrides the length) and the curve (D_FADE*DIR = 0), so the placed fade is the drawn one. Nothing
// for a variation out of range or a folder-pile source (its members are copied as they are).
struct FadeApply {
    bool   set_in = false;
    bool   set_out = false;
    bool   shape_in = false;   // write C_FADEINSHAPE
    bool   shape_out = false;  // write C_FADEOUTSHAPE
    double in = 0.0;
    double out = 0.0;
    int    in_shape = 0;
    int    out_shape = 0;
};
FadeApply FadeApplyFor(const SoundData& data, int variation, bool folder_source = false);
// The fades as written on an item of `item_len` s (playrate already applied to the length; the fade
// lengths are scaled by 1 / playrate like the item): each side clamped so in + out <= item_len.
// `cur_in` / `cur_out` = the item's fades as copied, used for a side that `fa` leaves alone.
void FadesOnItem(const FadeApply& fa, double playrate, double item_len, double cur_in, double cur_out,
                 double* in, double* out);

// The placed item's position (project s): the event, minus the entry's sync offset, plus the
// Blend offset (ms) and the user's nudge (s); clamped at 0.
double PlacementPosition(double event_t, double sync_offset, double blend_ms, double hand_offset_s);
// The item volume (D_VOL, linear) for a gain in dB (NaN -> 1).
double ItemGainFor(double gain_db);

// 13-10 (Antho 2026-10-12): why the engine refuses to move (`edit` false) or replace / remove
// (`edit` true) a placed item; "" = allowed. An attached sound is never replaced or removed.
std::string PlacedItemRefusal(bool found, bool has_tag, bool folder, Ownership own, bool edit);

// 13-11 (B9): an older build imported a User Library File Sound muted and marked a library source
// (kLibSourceTagKey). A copy of such an item is placed unmuted; any other source's mute state is
// copied as is.
bool PlacedCopyMuted(bool src_muted, bool src_is_library_source);
// Antho 2026-10-13: the volumes of a placed copy. The level heard in RAV is the level placed, so a copy
// plays at the library's gain alone (item D_VOL = gain, its take at unity with its polarity kept: the
// take's volume is in the library's gain, ItemGainDb). A folder pile keeps its members' volumes (the
// balance inside the pile): item = source item x gain, take untouched (set_take false).
struct PlacedVolumes {
    double item = 1.0;
    double take = 1.0;
    bool   set_take = false;
};
PlacedVolumes PlacedCopyVolumes(bool folder, double src_item_vol, double take_vol, double gain);
// Antho 2026-10-12 (13-11): a File Sound is placed straight from its file, no source item on the
// timeline. Its variation is its frame in every mode (a file has one take): `set` false = the whole
// file (a frame out of range or empty).
SliceApply FileSliceFor(const SoundData& data, int variation);
// The fades of a File Sound's placed item: both sides always (the frame's own or the Sound's default,
// the Sound's default for a variation out of range), with their shapes.
FadeApply FileFadeApplyFor(const SoundData& data, int variation);
// The item placed from a file: take D_STARTOFFS, item D_LENGTH, D_SNAPOFFSET (= the sync), D_POSITION
// (PlacementPosition), D_VOL (the gain), D_FADEINLEN / D_FADEOUTLEN (FadesOnItem, playrate 1).
// `file_len` = the file's length in s (<= 0 = unknown: the slice is taken as is). The slice is
// clamped into the file; `ok` false when nothing of it is left (or no slice and no length).
struct FileItemFields {
    bool   ok = false;
    double position = 0.0;
    double length = 0.0;
    double start_offs = 0.0;
    double snap_offset = 0.0;
    double volume = 1.0;
    double fade_in = 0.0;
    double fade_out = 0.0;
};
FileItemFields FileItemFieldsFor(double file_len, const SliceApply& slice, const FadeApply& fades, double gain_db,
                                 double project_t, double blend_ms, double offset_s);
// ---- 13-20 / 13-23 / 13-25: pieces (spec 13-20-23-25 D7) -----------------------------------------
// One placed item of a variation's part: a span of PlacedSpans (a part cut by an excluded zone gives 2
// pieces, placed together with the gap left empty). Every piece of one placement is positioned from
// the same anchor: the piece holding the sync point, whose snap offset puts the sync on the event.
struct PieceApply {
    SliceApply slice;          // D_STARTOFFS = the span's start, length (source s); snap_offset (source s)
                               // = sync - span start on the anchor piece, 0 on the others
    FadeApply  fades;          // the frame's fades on a frame-edge side, the Sound's cut_fade on a side at
                               // a cut or a zone edge
    bool       anchor = false; // holds the sync point
    double     from_anchor = 0.0;  // its span start minus the anchor piece's (source s)
};
// The pieces of variation `variation` playing part `part_value` ("" = the whole variation minus its
// exclusions), in time order. Fades at a frame edge: FadeApplyFor (`file` false) or FileFadeApplyFor
// (`file` true). The sync is the stored one (SyncFor) when it lies in the spans, else AutoSync without
// peaks (the first span's start + the offset, clamped into the spans). {} for Takes mode (A8: the
// take keeps its item's snap), a bad index, or nothing left to play (an empty or excluded part).
std::vector<PieceApply> PieceApplyFor(const SoundData& data, int variation, const std::string& part_value,
                                      bool file = false);
// Where a piece lands: the anchor piece's position plus its offset from it in item seconds
// (from_anchor / playrate; a bad rate reads as 1), clamped at 0.
double PiecePosition(double anchor_pos, double from_anchor, double playrate);

// The name of a copy of `file_name` in the project media folder: n = 0 the name itself, n >= 1
// "stem-001.ext" (REAPER's own numbering), the next free one when a different file has the name.
std::string MediaCopyName(const std::string& file_name, int n);

}  // namespace rav
