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

// One placement: the entry (a pile of items, by its top item's GUID, or a file) copied to the
// event's position.
struct PlaceRequest {
    MediaItem*  owner = nullptr;  // the animation item (null: dev action only; the tag's owner =
                                  // the source GUID, or the kept tag's owner on replace)
    std::string rid;
    double      event_t = 0.0;    // clip seconds
    double      project_t = 0.0;  // the event's project time
    std::string node;
    Ownership   own = Ownership::Populated;
    SoundSource source;  // Item: the entry's top item; File: the file
    int         variation = -1;
    SliceApply  slice;
    double      gain_db = 0.0;
    double      blend_ms = 0.0;
    double      offset_s = 0.0;
    MediaTrack* track = nullptr;  // nullptr = the entry's own tracks
};
struct PlaceResult {
    bool                     ok = false;
    std::vector<std::string> item_guids;  // the items placed (tagged)
    std::string              error;
};
PlaceResult PlaceSounds(ReaProject* proj, const PlaceRequest& req);
bool        MovePlacement(ReaProject* proj, const std::string& item_guid, double project_t);
PlaceResult ReplacePlacement(ReaProject* proj, const std::string& item_guid, const PlaceRequest& req);
bool        RemovePlacement(ReaProject* proj, const std::string& item_guid);
// REAPER's "copy imported media to project folder" preference (spike 13-0 D: copyimpmedia).
bool ReadCopyImportedMedia();

}  // namespace rav

#endif  // _WIN32
