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

#include "library_tree_edit.h"  // MissingSound
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
// Locate: pick the Sound's file (the dialog titled with its name, opened in its old folder); the
// other missing sounds are looked for through the same folders. `allow_rename` (a Sound's own
// Locate): a file of another name relinks that Sound to it (13-50 review); otherwise refused.
void RequestUserLibraryLocate(const std::string& lib_uid, const std::string& node_uid, bool allow_rename = false);
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

// ---- Spec missing-sounds-window: the one missing rule ---------------------------------------------
// An item Sound whose REAPER item is not in the current project.
bool ItemSoundGone(const SoundNode& n);
// Where a Sound's file is on disk ("" = not found): a File Sound's file, an item Sound's remembered
// file (its asset; "" when it never remembered one). An item Sound whose item is still there plays the item.
std::string SoundNodeFilePath(const SoundNode& n);
// The one rule (library_tree_edit.h SoundIsMissing): a File Sound whose file is not found; an item Sound
// whose item is gone and whose remembered file is not found (or unknown). What the red count, its
// tooltip, the tree's "missing" tag, the container counts, Populate and the Missing sounds window read.
bool SoundNodeMissing(const SoundNode& n, MissingWhy* why = nullptr);
std::vector<MissingSound> ListMissingSounds(const SoundLibrary& lib);
// Every library's, in the store's order.
std::vector<MissingSound> ListAllMissingSounds();
// What the item's active take plays as a remembered file (name, size, length, last path); empty when
// it plays no file (a folder item, an empty or MIDI take).
AssetIdentity ItemFileAsset(MediaItem* item);
// "Search a folder..." of the Missing sounds window: picks a folder (timer), then every missing sound of
// every library is looked for under it and its subfolders (by identity; an older item Sound with no
// remembered file by its name); one undo point; the toast says how many were found.
void RequestMissingSearchFolder();
// The folder searched last this session ("" = none).
const std::string& MissingLastSearchedFolder();
// The Sounds whose file was found by their name only (since ForgetFoundByName): the window marks them
// "Found by name: check it".
bool SoundFoundByName(const std::string& lib_uid, const std::string& node_uid);
void ForgetFoundByName();

}  // namespace rav

#endif  // _WIN32
