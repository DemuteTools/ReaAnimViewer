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

#include <cstdint>
#include <string>
#include <vector>

#include "reaper_api.h"
#include "sound_model.h"

namespace rav {

void DrawUserLibraryNodeSection();

// 13-7a (audit C1.4): user libraries are listed in the Libraries list under USER (library_tree_ui.cpp);
// this module keeps their folder, files and pickers.
// Once a frame before the list: installs the tree's missing hook, refreshes the caches, lists the
// folder (again when its files change).
void PrepareUserLibraries();
// A .ravlib of the user folder whose library is not in this project (unreadable ones included).
struct UserLibraryFile {
    std::string file;      // file name
    std::string name;      // its library's name (the file's stem when unreadable)
    uint32_t    color = 0;
    bool        readable = false;
};
std::vector<UserLibraryFile> UserFilesNotInProject();
// Adds that file's library to the project (one undo point) and selects it.
void AddUserLibraryFile(const std::string& file);
// "+ User Library": a new library saved to its own file in the folder (one undo point).
void NewUserLibrary();
// 13-24: "Convert to User Library" on a Project Library: in place, its Item Sounds that play a plain
// media file become File Sounds of it (still placed from their item in this project), the library
// is saved at once to its own .ravlib (one undo point; the toast lists the project-only Sounds).
// Call outside the ImGui frame's widgets (Later).
void ConvertToUserLibrary(const std::string& lib_uid);
// The pickers (run by this module's timer, never inside the frame).
void RequestUserLibraryImport();
void RequestUserLibraryExport(const std::string& lib_uid);
void RequestUserLibraryLocate(const std::string& lib_uid, const std::string& node_uid);
// 13-38: "Locate in a folder...": picks a folder (timer), then every missing File Sound of every
// library is looked for under it and its subfolders on the worker; one undo point "RAV: Locate sounds".
void RequestLibraryLocateFolder();
// 13-36: the User Library's .ravlib selected in a File Explorer window (its folder when the file is
// not written yet; the autosave runs first). Run by the timer, like the pickers.
void RequestUserLibraryReveal(const std::string& lib_uid);
// 13-50 (13-11): a Locate scan is running; Cancel stops it (its results are dropped, nothing changes).
bool UserLibraryLocating();
void CancelUserLibraryLocate();
// While a Locate scan runs: "Locating missing sounds..." and [Cancel] (draws nothing otherwise).
void DrawLibraryLocateStatus();
// 13-50 (13-7b review): a missing File Sound's [Locate file...] [Locate in a folder...] (the status
// and Cancel while a scan runs). The one place they are drawn: the Sound panel's red Source line.
void DrawLocateButtons(const std::string& lib_uid, const std::string& node_uid);
// A library's missing File Sounds (any library, Project Libraries included); `first` = the first
// one's node uid.
int UserLibraryMissing(const std::string& lib_uid, std::string* first);
// Lists the folder again and forgets the found / missing files.
void RefreshUserLibraries();
std::string UserLibraryFolderText();

// Resolves this module's functions and registers its timer (pickers, Locate result, autosave).
void InitLibraryUser(reaper_plugin_info_t* rec);
// Unload: unregisters the timer and joins the Locate worker.
void QuitLibraryUser();

// Where Populate places a File Sound from (Antho 2026-10-12, 13-11: straight from the file, no
// source item on the timeline).
struct FileSoundTarget {
    std::string path;             // the file (FileForProject: its project copy when REAPER copies imports)
    MediaTrack* track = nullptr;  // the "layer 1" track of its Lands-in folder (decision 12; 13-16:
                                  // a Blend's layers have their own tracks, SoundTrackPath)
    std::string item_guid;        // an older build's imported source item, used only when the file is gone
};
// Readies a File Sound for placing: resolves its file, copies it into the project media folder once
// when copyimpmedia is on, finds or makes its folder and layer track (no undo point of its own: call
// inside a SoundUndoScope). With no file but an older build's source item still in the project, that
// item. False = missing (`why` says why). `make_track` false (13-13: the caller has its Switch-named
// track): the file only, `track` left null (no unused "layer N" track made).
bool PrepareFileSound(ReaProject* proj, const std::string& lib_uid, const std::string& node_uid,
                      FileSoundTarget* out, std::string* why, bool make_track = true);
// A File Sound whose file cannot be found and that has no older imported item left (decision 10).
bool FileSoundMissing(const SoundNode& n);

}  // namespace rav

#endif  // _WIN32
