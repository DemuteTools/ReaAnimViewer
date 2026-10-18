// SPDX-License-Identifier: MIT
//
// Placement engine (13-6, REAPER): copies entry items onto the timeline and tags them (D4).
// No undo inside: callers wrap a gesture in SoundUndoScope (sound_project.h).
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-6
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32

#include <functional>
#include <string>
#include <vector>

#include "reaper_api.h"
#include "sound_model.h"
#include "sound_place_plan.h"

namespace rav {

// Resolves this module's extra REAPER functions (D7).
void InitSoundPlace(void* (*get_func)(const char* name));

// What placement reads of an item.
struct ItemFacts {
    std::string     guid;
    std::string     track_guid;
    double          position = 0.0;
    double          length = 0.0;
    double          snap_offset = 0.0;
    double          volume = 1.0;    // item D_VOL
    double          playrate = 1.0;  // active take D_PLAYRATE
    int             takes = 0;
    int             cur_take = 0;
    FolderItemFacts folder;
};
bool ReadItemFacts(MediaItem* item, ItemFacts* out);
bool IsFolderItem(MediaItem* item);
// 13-68 v3: the playrate of `item`'s take `k` (Takes mode: the variation's take; -1 or out of range: the active
// take); 1 when unreadable.
double TakePlayrateOf(MediaItem* item, int k);
// 13-68 v2: the items a folder pile places (its child items inside its span, as Populate collects them:
// nested folder items and items RAV placed left out). Empty when `item` is not a folder item.
std::vector<MediaItem*> FolderPileMembers(MediaItem* item);

// One placement: the entry (a pile of items, by its top item's GUID, copied) or a file (Antho
// 2026-10-12, 13-11: an item made straight from `file_path` on `track`, no source item) at the
// event's position.
struct PlaceRequest {
    MediaItem*  owner = nullptr;  // the animation item (null: dev action only; the tag's owner =
                                  // the source GUID, or the kept tag's owner on replace)
    std::string rid;
    double      event_t = 0.0;    // clip seconds
    double      project_t = 0.0;  // the event's project time
    std::string node;
    Ownership   own = Ownership::Populated;
    SoundSource source;  // Item: the entry's top item; File: the file (`file_path`, `track` required)
    std::string file_path;  // File: the file to place, as FileForProject gave it
    int         variation = -1;
    // 13-20 / 13-23 / 13-25 (D7): the pieces of the variation's part (PieceApplyFor), one item each,
    // all positioned from the anchor piece and tagged with ao / sy like a pile's members; empty = one
    // item from `slice` (Takes, a folder pile ignores them). `part` = the part played (the tags' pt).
    std::vector<PieceApply> pieces;
    std::string part;
    SliceApply  slice;
    FadeApply   fades;  // the variation's fades (FadeApplyFor); nothing set = the copy's own
    double      gain_db = 0.0;
    double      blend_ms = 0.0;
    double      offset_s = 0.0;
    MediaTrack* track = nullptr;  // nullptr = the entry's own tracks (Item only)
    // Story 13-16: `track` is a RAV track carrying this mark (sound_tracks.h): the item never
    // overlaps another there (it spills onto the track's children, SettleOnLanes) and its tag
    // records the mark of the track it lands on (PlacedTag::tk). "" = neither (the source's track).
    std::string track_mark;
    // B5: an NVK pile's member tracks: given a member's source child track, its destination track
    // and that track's mark (nullptr = the member's own track). Unset = as `track` says.
    std::function<MediaTrack*(MediaTrack* source_track, std::string* mark)> pile_track;
    // 13-29 / 13-41 (Antho 2026-10-13: what RAV plays is what is placed): a library Sound's copy plays its
    // file as the audition does: every take of a copied plain item gets playrate 1 (its length kept in
    // source seconds), pitch 0, pan centre, no take FX and channel mode `chan_mode` (ChanModeFor); an item
    // placed from a file gets `chan_mode`. False = the copy as it is (a Spread copy). A folder pile's members
    // are always copied as they are.
    bool as_auditioned = false;
    int  chan_mode = 0;
    // 13-68 v3 (SoundData::keep_rate): with as_auditioned, a plain item's copy keeps its takes' playrate, pitch and
    // preserve-pitch as the source has them (FoldPlayrate keep); its slices, pieces, fades and snap are read at that rate.
    bool keep_rate = false;
    // 13-68 v2: a TakeMarkers Sound's variation index: a folder pile's members each play their own slice of
    // that index (their take markers / NVK slices, modulo their count), as a Takes pile plays take k. -1 = none.
    int  pile_slice = -1;
};
struct PlaceResult {
    bool                     ok = false;
    std::vector<std::string> item_guids;  // the items placed (tagged)
    std::vector<MediaItem*>  items;       // the same items, valid until the gesture ends
    std::string              error;
};
PlaceResult PlaceSounds(ReaProject* proj, const PlaceRequest& req);
bool        MovePlacement(ReaProject* proj, const std::string& item_guid, double project_t);
// The kept item is moved to the request's track, except (13-16 B7b) when its tag records a RAV
// track it is no longer on: the user moved it, it stays where it is.
PlaceResult ReplacePlacement(ReaProject* proj, const std::string& item_guid, const PlaceRequest& req);
bool        RemovePlacement(ReaProject* proj, const std::string& item_guid);
// Story 13-26 (Clear item data): deletes a placed item whatever its ownership, attached by the user
// included: the one gesture allowed past RemovePlacement's "attached" guard. Still only an item that
// carries a RAV_SND tag, never a folder item. The caller found it (one project scan for the gesture);
// it leaves the gesture's caches (ForgetLiveItem included) before it goes. False when not deleted.
bool        RemovePlacedAny(ReaProject* proj, MediaItem* item);
// Where a placed pile is anchored, from any one of its placed items (its tag's ao / sy, 13-8):
// `anchor_pos` = the position its anchor lands at, `sync` = the snap it was placed by. A tag
// without them: the item's own position and snap. False when `item` is null.
bool PlacedPileAnchor(MediaItem* item, double* anchor_pos, double* sync);
// 13-62h: the pieces of one placement that overlap in a Gap's crossfade count as butted for the lane rules
// (SettleOnLanes, FixLaneOverlap: they stay on one track). While it lives, each item that starts before the end
// of the one before it on its track (by position) starts at that end instead, its length shortened by as much; put back as
// they were when it goes. Items that do not overlap are left alone.
class CrossfadeAsButted {
public:
    explicit CrossfadeAsButted(const std::vector<MediaItem*>& items);
    ~CrossfadeAsButted();
    CrossfadeAsButted(const CrossfadeAsButted&) = delete;
    CrossfadeAsButted& operator=(const CrossfadeAsButted&) = delete;

private:
    struct Was {
        MediaItem* item;
        double     pos, len;
    };
    std::vector<Was> was_;
};
// A gesture's GUID -> item lookup (no scan; null = none) the engine asks before scanning the
// project; a hit is checked with ValidatePtr2. Null clears it.
void SetPlaceItemFinder(MediaItem* (*finder)(const std::string& guid));
// Antho 2026-10-12 (Populate was slow, sounds appearing one by one): a gesture's batch. On, REAPER's arrange
// does not redraw (PreventUIRefresh) and each source item's chunk is read once (sources are not changed by a
// gesture); off (nested calls count) redraws once and forgets the chunks.
void PlaceBatch(bool on);
// REAPER's "copy imported media to project folder" preference (spike 13-0 D: copyimpmedia).
bool ReadCopyImportedMedia();
// The path a file is placed from in `proj` (Antho 2026-10-12, 13-11): with copyimpmedia on, its copy
// in the project media folder (GetProjectPathEx), made the first time (a copy already there of the
// same size is reused; a different file of that name gives "name-001.ext"...); else, or when the file
// already sits in that folder or the copy fails, the file itself.
std::string FileForProject(ReaProject* proj, const std::string& path);

// 13-35 (Sync placed sounds): a placed item brought to the Library's settings in place -- nothing copied again, so
// its takes, FX and the user's other edits stay. `piece` = its piece of the request (its span, its snap on the anchor
// piece, its fades); else `slice` / `fades` (a sound placed as one item); neither for a folder pile's member (its volume
// alone). `set_length` / `set_fades` false = the item's own span / fades kept (the user's hand edit). D_VOL = `vol`.
struct PlacedUpdate {
    const PieceApply* piece = nullptr;
    const SliceApply* slice = nullptr;
    const FadeApply*  fades = nullptr;
    bool              set_length = true;
    bool              set_fades = true;
    double            vol = 1.0;
};
void UpdatePlacedItem(MediaItem* item, const PlacedUpdate& u);
// What a placed item holds now: D_VOL, its fades (length, shape, curvature) and D_LENGTH (PlacedTag::wr).
PlacedTag::Written ReadPlacedWritten(MediaItem* item);
// Moves a placed item (D_POSITION, clamped at 0).
void SetPlacedPosition(MediaItem* item, double pos);
// Sets a placed item's D_VOL (13-35: a hand edit kept on a sound placed again).
void SetPlacedVolume(MediaItem* item, double vol);

}  // namespace rav

#endif  // _WIN32
