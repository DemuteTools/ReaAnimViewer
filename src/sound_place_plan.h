// SPDX-License-Identifier: MIT
//
// Placement plan (13-6, pure): where and how a placed item lands.
// Epic 13 (D20). Created by 13-2 with declared interfaces; story 13-6 filled it.
// Shared types: sound_model.h.

#pragma once

#include <cmath>
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
// overrides the length) and sets the curve (13-75: the shape, then its curvature; no shape: D_FADE*DIR = 0), so the
// placed fade is the drawn one. Nothing
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
    // 13-75: the curvatures written with a shape (D_FADEINDIR / D_FADEOUTDIR, after the shape: REAPER sets the shape's
    // preset with it); NaN = that preset (FadeDirOr).
    double in_dir = NAN;
    double out_dir = NAN;
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
// 13-41: the take channel mode (I_CHANMODE) a placed item of a Sound forced to `mono` gets: Off 0
// (normal), Sum 2 (mono downmix), Left 3, Right 4.
int ChanModeFor(MonoMode mono);
// 13-49 review: the Sound's mono a take channel mode reads as (a Sound made from an item whose take
// plays mono is heard mono in RAV too): 2 Sum, 3 Left, 4 Right, anything else Off (Reverse, normal...).
MonoMode MonoForChanMode(int chan_mode);
// 13-49 review: a placed copy plays at playrate 1, its active take's rate folded into the item: the item's
// length and snap in source seconds (as the variation was measured). A rate that is not > 0 or not finite
// reads 1 (nothing folded).
// 13-68 v3: `keep` (SoundData::keep_rate: a copy tuned by ear keeps its take's rate, pitch and preserve-pitch):
// nothing folded, `reset` false, the length and snap stay in item seconds at that rate.
struct PlayrateFold {
    double rate = 1.0;    // the rate folded (sanitized)
    double length = 0.0;  // the item's new D_LENGTH
    double snap = 0.0;    // its new D_SNAPOFFSET
    bool   reset = true;  // the takes go to playrate 1 / pitch 0
};
PlayrateFold FoldPlayrate(double rate, double item_len, double snap, bool keep = false);
// 13-68 v3: a take at `rate` (not > 0 or not finite reads 1) plays `item_s` item seconds of its source as item_s x rate
// source seconds, and a source span `source_s` (a variation, a sync) over source_s / rate item seconds.
inline double SaneRate(double rate) { return (std::isfinite(rate) && rate > 0.0) ? rate : 1.0; }
inline double SourceSecondsOf(double item_s, double rate) { return item_s * SaneRate(rate); }
inline double ItemSecondsOf(double source_s, double rate) { return source_s / SaneRate(rate); }
// 13-49 review: an item chunk as a placed copy that plays its file plain gets it: every take's <TAKEFX
// block (with its nested blocks), TAKE_FX_HAVE lines and stretch markers (SM lines) removed at the item
// level; every other line byte-identical (no plugin is loaded for a copy). A chunk without them is unchanged.
std::string StripTakeFxAndStretch(const std::string& chunk);
// 13-49 review: the Sound panel's read-only line when the source item carries what placement drops:
// "Placed without the source item's take FX, pitch and rate (RAV plays the file itself)"; "" for none.
std::string PlacementDropsText(bool take_fx, bool pitch, bool rate, bool pan);

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
// 13-62i (D, P2): a placed item a re-Populate keeps follows the library's gain now. `piece_db` = the dB each
// piece of its slot plays at now (DesiredSlot::piece_db), `items` = how many items the slot has, `pile` = a
// folder pile. A tagged item (has_g: its tag's g, pi) gets D_VOL x 10^((want - g) / 20), want = piece_db[pi]
// (the user's own change of its volume kept); an older tag (no g) is set to ItemGainFor(want) only when it
// is its slot's one item, not of a pile, and the slot has one piece; anything else is left. `set` false =
// nothing to write (also when want == g within 1e-9 dB). Then `g` / `pi` are what its tag records.
struct KeptVolume {
    bool   set = false;
    double vol = 1.0;
    double g = 0.0;
    int    pi = 0;
};
KeptVolume KeptItemVolume(const std::vector<double>& piece_db, bool pile, int items, bool has_g, double g, int pi,
                          double cur_vol);
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
    double     from_anchor = 0.0;  // its span start minus the anchor piece's (source s; 13-62h: heard time)
    double     db = 0.0;           // 13-62h: its zone's own gain (dB), on top of the placement's gain
};
// The pieces of variation `variation` playing part `part_value` ("" = every part zone), in time order. 13-62i: every
// Sound is placed from its zones (Takes mode too: its take's zones; a Sound read but not laid out is laid out on a copy
// first, ZonesLaidOut); `file` no longer changes anything. {} for a bad index, an off variation, or a variation with no
// zone of that part.
// 13-62h (spec 13-62h Lot B, C2): one piece per part zone, never merged (a split Heel|Heel = 2 butted items). Each
// piece is cut as HeardPieces cuts it: at a Gap it runs into the Gap by xl on its right / starts xr before the Gap's end
// on its left, its fade on that side = xl + xr (the two items overlap by xl + xr and crossfade); every other side
// ZoneFade (own -- with its shape, 13-62i -- / At cuts / part / Sound). Positions are heard time: a piece after a Gap
// comes the jumped length earlier than in the file; empty space stays. The anchor = the first piece with a snap of its
// own, else the first; its snap = ZoneSnap of its zone (`peaks` when given, PieceSnapNeedsPeaks). `db` = its zone's gain
// (the lane fader is not added: it already shifted the zone gains, C3). 13-62h review (R3): a side whose Gap leads to a
// zone this placement does not place (another part's) is cut at the Gap's edge with the At cuts fade (ZoneCutFade), its
// position still in heard time. Fades never rescaled (GapReach).
std::vector<PieceApply> PieceApplyFor(const SoundData& data, int variation, const std::string& part_value,
                                      bool file = false, const PeakData* peaks = nullptr);
// 13-62h: true when the anchor piece of a zoned placement (PlacedFromZones) reads the file's peaks for its snap
// (its zone has no snap of its own and its part's Sync point is Attack or Peak); false for a Sound not edited as zones.
bool PieceSnapNeedsPeaks(const SoundData& data, int variation, const std::string& part_value);
// 13-62i: the gain (dB) a folder pile is placed at: a pile ignores its pieces, so the placement's gain plus the zone gain
// of its anchor piece (else its first; none: the gain as given). Its zone's gain is where the variation's gain lives.
double PileGainDb(const std::vector<PieceApply>& pieces, double gain_db);
// Where a piece lands: the anchor piece's position plus its offset from it in item seconds
// (from_anchor / playrate; a bad rate reads as 1), clamped at 0.
double PiecePosition(double anchor_pos, double from_anchor, double playrate);

// The name of a copy of `file_name` in the project media folder: n = 0 the name itself, n >= 1
// "stem-001.ext" (REAPER's own numbering), the next free one when a different file has the name.
std::string MediaCopyName(const std::string& file_name, int n);

}  // namespace rav
