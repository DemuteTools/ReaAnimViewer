// SPDX-License-Identifier: MIT
//
// The tracks RAV places sounds on (REAPER side): the Lands-in folders' tracks (13-11, decision 12,
// moved here from library_user_ui.cpp) and the Switch-named tracks (story 13-13; the plan is
// sound_tracks_plan.h). RAV finds a track by name under its parent and makes it only when missing;
// it never renames, deletes or reorders one. No undo point of its own: call inside a
// SoundUndoScope.

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

// The track at the end of a track path (TrackPathFor), found or made level by level (T3): the top
// level is a folder track named exactly so anywhere in the project, else a new folder at the
// bottom; each next level is the parent's direct child that MatchChildTrack finds (the second
// level, the library folder, among the children that are folders or Switch-tree folders), else a
// new last child. The library folder is marked a Switch-tree folder (IsSwitchTreeTrack). nullptr
// when the path is empty or a track could not be made (`failed` = its name).
MediaTrack* EnsureTrackPath(ReaProject* proj, const std::vector<TrackStep>& path, std::string* failed = nullptr);

}  // namespace rav

#endif  // _WIN32
