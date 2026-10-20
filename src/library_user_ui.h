// SPDX-License-Identifier: MIT
//
// Library view (13-11): User Libraries (.ravlib) in column 1 and the node panel.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-11
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// The user folder is <resource>/ReaAnimViewer/Libraries/. 13-77a: every library lives in the project and is
// linked to a .ravlib of that folder or not; a file is written only by an explicit gesture (no autosave). Populate places File Sounds straight
// from their file (PrepareFileSound, Antho 2026-10-12: no source item); Locate's folder work runs on a
// worker thread. Every REAPER function beyond reaper_api.h is resolved in InitLibraryUser
// (missing = that part greys out).

#pragma once

#ifdef _WIN32

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "library_sync.h"  // 13-77b
#include "library_tree_edit.h"  // MissingSound
#include "ravlib.h"  // LinkedFileState
#include "reaper_api.h"
#include "sound_model.h"

namespace rav {

// 13-77a: the node panel's File section (where the library's file stands: Save to a file..., the link, the red
// "file not found" bar) and Tracks section (Lands in, every library but an Attached one).
void DrawLibraryFileSection();
void DrawLibraryTracksSection();

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
// 13-77a "Save to a file...": an unlinked library is written as a .ravlib of the folder and linked to it (one undo
// point; the 13-24 conversion of its Item Sounds first, the toast lists the project-only ones). A failed write
// changes nothing. Call outside the ImGui frame's widgets (Later).
void SaveLibraryToFile(const std::string& lib_uid);
// "Unlink from file": one undo point; the library stays in the project, the file is not touched.
void UnlinkLibrary(const std::string& lib_uid);
// Run by this module's timer (for the File section's buttons, drawn inside the frame).
void RequestSaveLibraryToFile(const std::string& lib_uid);
void RequestSaveLibraryFileAgain(const std::string& lib_uid);
// "Locate..." on a library whose file is gone: picks a .ravlib that must hold this library (the picker runs on the timer).
void RequestRelinkLibrary(const std::string& lib_uid);
// "Save it again": this project's copy written as the file; the common version = what was written.
void SaveLibraryFileAgain(const std::string& lib_uid);
// Where a linked library's file stands (the folder listing, no disk access): Ok / Missing / OtherLibrary /
// Unreadable; Unlinked for the others.
LinkedFileState LinkedFileOf(const SoundLibrary& lib);
// ---- 13-77b: the library <-> file changes (library_sync.h). Every read of the file is on a worker; these answer from the
// last read (null / not ready until it came back). The file is read when the project opens, when REAPER comes back to the
// foreground and before each Apply; nothing is ever taken or written without a gesture.
// The changes of a linked library (null: not read yet, unreadable, or not linked).
const SyncDiff* LibrarySyncDiff(const std::string& lib_uid);
// 13-77c (review 13-77b L5): how an Apply ended. Stopped = the file moved meanwhile (nothing written, the list is rebuilt:
// the window stays open, its ticks kept by key); Partly = applied, some picked changes could not be (they stay listed).
enum class SyncApplyOutcome { None, Applied, Partly, Nothing, Stopped, Failed };
struct LibrarySyncInfo {
    bool        ready = false;    // a diff is there
    bool        bad = false;      // the file could not be read just now
    bool        reading = false;  // a read is awaited
    std::string by;               // who wrote the file (its header), when
    int64_t     at = 0;
    int64_t     synced_at = 0;    // this project's last sync (unix s), 0 = never
    uint64_t    apply_seq = 0;    // moves each time an Apply of this library ends (in this project)
    SyncApplyOutcome last_apply = SyncApplyOutcome::None;
};
LibrarySyncInfo LibrarySyncInfoOf(const std::string& lib_uid);
// "Send mine" (Up) / "Get file's" (Down) for one row: a container's changes (its own, the ones under containers the
// project does not have yet, and for the root the Switch Group ones: SyncChange::row) or one Sound's own, dependencies
// included. Queued: run by the timer, one Apply = one undo point `RAV: Sync with file`; the file is read again first and
// nothing is written when it moved.
void RequestNodeSync(const std::string& lib_uid, const std::string& node_uid, SyncDir dir);
// The same for an exact selection of change keys (13-77c's window); the selection is closed (needs / dependents) by the
// Apply. `diff_sig` (optional, SyncDiffSignature of the diff the picks were made on): the Apply stops (Stopped, toast "the
// changes moved") when the list it meets is another. `done` (optional) is called once with how it ended, on the UI thread (even when it could not start).
void RequestSyncApply(const std::string& lib_uid, const std::map<std::string, SyncDir>& picks,
                      std::function<void(SyncApplyOutcome)> done = {}, uint64_t diff_sig = 0);
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
// 13-36: the linked library's .ravlib selected in a File Explorer window (its folder when the file is
// not written). Run by the timer, like the pickers.
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

// Resolves this module's functions and registers its timer (pickers, Locate result, first-sight common versions).
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
