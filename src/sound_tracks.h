// SPDX-License-Identifier: MIT
//
// The tracks RAV places sounds on (REAPER side): the Lands-in folders' tracks (13-11, decision 12,
// moved here from library_user_ui.cpp) and the Switch-named tracks (story 13-13; the plan is
// sound_tracks_plan.h). RAV finds a track by its hidden mark (13-16; the Lands-in folder by name)
// and makes it only when missing; it renames the marked tracks the user has not renamed (13-17)
// and never deletes or reorders one. No undo point of its own: call inside a
// SoundUndoScope. Story 13-16: tracks RAV makes for sounds carry a hidden mark (P_EXT:RAV_TRACK,
// built from uids, sound_tracks_plan.h) and are found by it; a placed item that would overlap
// another on a RAV track goes to a spill child instead.

#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

#include "reaper_api.h"
#include "sound_tracks_plan.h"

namespace rav {

// Resolves this module's REAPER functions (InsertTrackAtIndex, SetMediaTrackInfo_Value).
void InitSoundTracks(void* (*get_func)(const char* name));
// True when tracks can be made (the functions above resolved).
bool CanMakeTracks();

// The track's name ("" when none).
std::string TrackName(MediaTrack* tr);
// The track's index in the project (-1 when not there).
int TrackIndex(ReaProject* proj, MediaTrack* tr);
// The direct children of the folder at `fi`, its last descendant, and how many folders above `fi`
// that one also closes (FolderChildren, ravlib.h).
std::vector<int> DirectChildren(ReaProject* proj, int fi, int* last, int* beyond);
// A new track named so at index `idx` (nullptr when it could not be made).
MediaTrack* AddTrackAt(ReaProject* proj, int idx, const std::string& name);
// A new track named so, the parent's last child: after its last descendant, the folder depths
// fixed so every folder still closes where it did (a parent that was no folder becomes one).
MediaTrack* AddChildTrack(ReaProject* proj, MediaTrack* parent, const std::string& name);

// True when the track is a RAV Switch-tree folder: the library folder EnsureTrackPath found or made
// in a Lands-in folder (its P_EXT mark), never one of that folder's layer tracks (PickLayerChild).
bool IsSwitchTreeTrack(MediaTrack* tr);

// The track at the end of a track path (SoundTrackPath), found or made level by level (T3): the top
// level is a folder track named exactly so anywhere in the project, else a new folder at the
// bottom; each next level is the parent's direct child that MatchChildTrack finds (the second
// level, the library folder, among the children that are folders or Switch-tree folders), else a
// new last child. The library folder is marked a Switch-tree folder (IsSwitchTreeTrack). nullptr
// when the path is empty or a track could not be made (`failed` = its name).
//
// Story 13-16 (B7): a level with a mark (TrackStep::mark) is found by its mark anywhere in the
// project (the user may rename, move or reorder it), the deepest found level first; a missing one
// is made as the previous level's last child and marked. The Lands-in folder (no mark) is found by
// name as above, and only when no marked level below it was found.
MediaTrack* EnsureTrackPath(ReaProject* proj, const std::vector<TrackStep>& path, std::string* failed = nullptr);

// ---- Story 13-16: marks, NVK pile children, spill lanes ------------------------------------------

// A gesture's batch (sound_place.h PlaceBatch): the project's marks are read once and the lanes'
// items cached; off (nested calls count) forgets them. Outside a batch every call reads the project.
void TracksBatch(bool on);
// The track's RAV mark, else (a track RAV did not mark, such as 13-11's "layer N") GuidTrackMark
// of its GUID. "" for a null track.
std::string TrackMarkOf(MediaTrack* tr);
// The track that carries `mark` (a GuidTrackMark: the track of that GUID). nullptr = none.
MediaTrack* FindTrackByMark(ReaProject* proj, const std::string& mark);
// The child of `parent` marked `mark`, found anywhere by its mark, else made as `parent`'s last
// child named `name` and marked (B5). nullptr when it could not be made.
MediaTrack* EnsureMarkedChild(ReaProject* proj, MediaTrack* parent, const std::string& name, const std::string& mark);
// B6: puts `item` (just placed or replaced on `base`, the RAV track marked `base_mark`) where it
// overlaps no item: on `base` while nothing there overlaps it and `base` has no spill child yet;
// at the first overlap `base`'s items placed there by RAV (their tag's tk = `base_mark`) and
// `item` move to spill children "<base name>01", "02"... (13-17 N1; each on the first that fits); once
// spilled, on the first spill child that fits, else a new one. Returns the mark of the track it
// ends on (its tag's tk); the other items moved are re-tagged here. "" when it could not move it.
std::string SettleOnLanes(ReaProject* proj, MediaItem* item, MediaTrack* base, const std::string& base_mark);
// B7c: a placed item still on the RAV track its tag records that now overlaps an item there (a
// follow move) moves to a free spill child of the same track family, re-tagged. True when moved.
bool FixLaneOverlap(ReaProject* proj, MediaItem* item);
// The lane cache: an item about to be deleted is dropped; an item changed in place is read again.
void ForgetLaneItem(MediaItem* item);
void RefreshLaneItem(MediaItem* item);

// ---- Story 13-17: RAV track names follow the Library ----------------------------------------------

// N1/N2/N5: every track with a RAV mark takes the name the Library gives it (NameForMark), unless
// the user renamed it in REAPER (its name differs from the one RAV recorded on it, P_EXT
// RAV_TRACK_NAME, written when RAV makes or renames it). Tracks without a mark (the Lands-in folder,
// 13-11's "layer N") are never renamed. No undo point of its own. True when a track changed.
// InitSoundTracks hooks it on every store edit (SetSoundStoreEditHook): a rename in the Library is
// one undo point with the track names.
bool RefreshRavTrackNames(ReaProject* proj);

}  // namespace rav

#endif  // _WIN32
