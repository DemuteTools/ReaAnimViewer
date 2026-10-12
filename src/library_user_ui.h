// SPDX-License-Identifier: MIT
//
// Library view (13-11): User Libraries (.ravlib) in column 1 and the node panel.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-11
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// The user folder is <resource>/ReaAnimViewer/Libraries/. A User Library is written to its file
// whenever its text changes (autosave, checked on a timer). File Sounds are imported into the
// project the first time Populate uses them (MaterialiseFileSound); Locate's folder work runs on a
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

// The item a File Sound plays from in this project: its materialised item when still there, else
// the file is imported now (decision 12) and the new item's GUID stored in the Sound (no undo
// point of its own: call inside a SoundUndoScope). "" = missing (`why` says why).
std::string MaterialiseFileSound(ReaProject* proj, const std::string& lib_uid, const std::string& node_uid,
                                 std::string* why);
// A File Sound whose materialised item is gone and whose file cannot be found (decision 10).
bool FileSoundMissing(const SoundNode& n);

}  // namespace rav

#endif  // _WIN32
