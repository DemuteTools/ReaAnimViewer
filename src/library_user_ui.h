// SPDX-License-Identifier: MIT
//
// Library view (13-11): User Libraries (.ravlib) in column 1 and the node panel.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-11
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// The user folder is <resource>/ReaAnimViewer/Libraries/. A User Library is written to its file
// whenever its text changes (autosave, checked on a timer). Populate places File Sounds straight
// from their file (PrepareFileSound, Antho 2026-10-12: no source item); Locate's folder work runs on a
// worker thread. Every REAPER function beyond reaper_api.h is resolved in InitLibraryUser
// (missing = that part greys out).

#pragma once

#ifdef _WIN32

#include <string>

#include "reaper_api.h"
#include "sound_model.h"

namespace rav {

void DrawUserLibrarySection();
void DrawUserLibraryNodeSection();

// Resolves this module's functions and registers its timer (pickers, Locate result, autosave).
void InitLibraryUser(reaper_plugin_info_t* rec);
// Unload: unregisters the timer and joins the Locate worker.
void QuitLibraryUser();

// Where Populate places a File Sound from (Antho 2026-10-12, 13-11: straight from the file, no
// source item on the timeline).
struct FileSoundTarget {
    std::string path;             // the file (FileForProject: its project copy when REAPER copies imports)
    MediaTrack* track = nullptr;  // the layer track of its Lands-in folder (decision 12)
    std::string item_guid;        // an older build's imported source item, used only when the file is gone
};
// Readies a File Sound for placing: resolves its file, copies it into the project media folder once
// when copyimpmedia is on, finds or makes its folder and layer track (no undo point of its own: call
// inside a SoundUndoScope). With no file but an older build's source item still in the project, that
// item. False = missing (`why` says why).
bool PrepareFileSound(ReaProject* proj, const std::string& lib_uid, const std::string& node_uid,
                      FileSoundTarget* out, std::string* why);
// A File Sound whose file cannot be found and that has no older imported item left (decision 10).
bool FileSoundMissing(const SoundNode& n);

}  // namespace rav

#endif  // _WIN32
