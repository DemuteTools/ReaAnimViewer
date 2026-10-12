// SPDX-License-Identifier: MIT
//
// Placement engine (13-6, REAPER): copies entry items onto the timeline and tags them (D4).
// No undo inside: callers wrap a gesture in SoundUndoScope (sound_project.h).
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-6
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32

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
    SliceApply  slice;
    FadeApply   fades;  // the variation's fades (FadeApplyFor); nothing set = the copy's own
    double      gain_db = 0.0;
    double      blend_ms = 0.0;
    double      offset_s = 0.0;
    MediaTrack* track = nullptr;  // nullptr = the entry's own tracks (Item only)
};
struct PlaceResult {
    bool                     ok = false;
    std::vector<std::string> item_guids;  // the items placed (tagged)
    std::vector<MediaItem*>  items;       // the same items, valid until the gesture ends
    std::string              error;
};
PlaceResult PlaceSounds(ReaProject* proj, const PlaceRequest& req);
bool        MovePlacement(ReaProject* proj, const std::string& item_guid, double project_t);
PlaceResult ReplacePlacement(ReaProject* proj, const std::string& item_guid, const PlaceRequest& req);
bool        RemovePlacement(ReaProject* proj, const std::string& item_guid);
// Where a placed pile is anchored, from any one of its placed items (its tag's ao / sy, 13-8):
// `anchor_pos` = the position its anchor lands at, `sync` = the snap it was placed by. A tag
// without them: the item's own position and snap. False when `item` is null.
bool PlacedPileAnchor(MediaItem* item, double* anchor_pos, double* sync);
// A gesture's GUID -> item lookup (no scan; null = none) the engine asks before scanning the
// project; a hit is checked with ValidatePtr2. Null clears it.
void SetPlaceItemFinder(MediaItem* (*finder)(const std::string& guid));
// REAPER's "copy imported media to project folder" preference (spike 13-0 D: copyimpmedia).
bool ReadCopyImportedMedia();
// The path a file is placed from in `proj` (Antho 2026-10-12, 13-11): with copyimpmedia on, its copy
// in the project media folder (GetProjectPathEx), made the first time (a copy already there of the
// same size is reused; a different file of that name gives "name-001.ext"...); else, or when the file
// already sits in that folder or the copy fails, the file itself.
std::string FileForProject(ReaProject* proj, const std::string& path);

}  // namespace rav

#endif  // _WIN32
