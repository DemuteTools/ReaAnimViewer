// SPDX-License-Identifier: MIT
//
// User Library files (13-11, pure): a .ravlib = the RAVSOUNDS grammar (sound_record.h) of one library
// and its Switch Groups under the magic "RAVLIB 1".
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-11
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// v1 file: line 1 "RAVLIB 1", then exactly SerializeSoundStore of a store holding the one library
// and the groups its Switches use, its header line dropped. The file never holds the library's
// ravlib_path nor a File source's item_guid (both project-local, spec 13-11 decision 5).
// v2 (13-23, spec 13-20-23-25 D8): the same grammar; the groups also include the ones its Sounds are
// divided by (with their values' roles), and Sound / frame / zone lines may hold the parts, zones and
// sync points. A v1 file reads with the same parser (its Sounds combined).

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sound_entries.h"
#include "sound_model.h"

namespace rav {

class MediaDbIndex;

constexpr const char kRavlibMagic[] = "RAVLIB";
constexpr int        kRavlibVersion = 2;  // the version SerializeRavlib writes; any >= 1 reads
constexpr const char kRavlibExt[] = ".ravlib";
// Where a User Library Sound lands when no node above it says (decision 12).
constexpr const char kDefaultLandsIn[] = "RAV Library";

// Reads a .ravlib into `out` (one library, its groups). False (and `out` untouched) when it is not one:
// no "RAVLIB <n>" (n >= 1) first line, or not exactly one library.
bool ParseRavlib(const std::string& text, SoundStore* out);
// Writes one library and the groups its Switches use (pass RavlibOf's result).
std::string SerializeRavlib(const SoundStore& one);
// 13-77a: the same, the header saying who wrote the file: "RAVLIB 2 by=<project file name> at=<unix s>" (an empty
// `by` / zero `at` is left out). Older readers keep the two fields as the header's unknown fields.
std::string SerializeRavlib(const SoundStore& one, const std::string& by, int64_t at);
// The by / at of a parsed .ravlib (its header's kept fields); "" / 0 when it has none.
void RavlibWriter(const SoundStore& parsed, std::string* by, int64_t* at);
// True when two .ravlib texts hold the same library, whoever wrote them and whenever (headers ignored, and a
// pre-13-77 `user=1` too). Unreadable texts compare by their lines after the first.
bool SameRavlibContent(const std::string& a, const std::string& b);

// The export form of one library of `store`: the library (ravlib_path cleared, File sources'
// item_guid cleared) and the groups its Switches use or its Sounds are divided by, in store order. `item_sounds` (optional)
// gets how many of its Sounds come from project items (missing anywhere else). An unknown uid
// gives an empty store.
SoundStore RavlibOf(const SoundStore& store, const std::string& lib_uid, int* item_sounds);

// Import merge (decision 7). False and `store` unchanged when `file` is not one library or its
// uid is already in the store (`why` "Already in this project", `lib_uid` = that uid).
// 13-23 (D2): a group new to the project comes with its values' roles; a group the project has keeps
// the project's role map (a value it adds brings only the role keys no value of that group holds).
// Divide groups and values, role maps, zones' parts and Blend layer parts follow the merge's uids.
// 13-77a: the library comes in linked to `file_name`. `uid_map` (optional) gets every uid the merge changed (file
// uid -> project uid: renewed uids and groups / values merged into the project's own); `base_out` (optional)
// gets the common version: the file's library in project uid space (lib_uid, text, uid_map, the file's by / at;
// `synced_at` is left for the caller).
bool AddRavlibLibrary(SoundStore& store, const SoundStore& file, const std::string& file_name, std::string* lib_uid,
                      std::string* why, std::vector<std::pair<std::string, std::string>>* uid_map = nullptr,
                      LinkBase* base_out = nullptr);

// ---- 13-77a: linked libraries (pure) -------------------------------------------------------------------

// One .ravlib of the User Library folder: its file name and its library's uid ("" = unreadable).
struct FolderRavlib {
    std::string file;
    std::string uid;
};
// The file of the folder that holds library `uid` ("" = none): Import refuses a library whose file is there.
std::string FolderFileOfUid(const std::vector<FolderRavlib>& folder, const std::string& uid);
enum class LinkedFileState { Unlinked, Ok, Missing, OtherLibrary, Unreadable };
// Where a library's file is: Unlinked (and Attached ones); Ok (the file is in the folder, holding this library);
// Missing (no such file); OtherLibrary (the file holds another uid); Unreadable. File names compare ignoring case.
LinkedFileState LinkedFileStateOf(const SoundLibrary& lib, const std::vector<FolderRavlib>& folder);
// File names compare the way the file system does: ignoring case, accented letters included ("\xC9t\xE9" and
// "\xE9t\xE9" are one file on NTFS). The host uses a built-in fold (ASCII, Latin-1 / Latin Extended-A, Greek,
// Cyrillic); the DLL installs the Win32 one (SetFileNameFold) so both agree with the disk.
using FileNameFoldFn = std::string (*)(const std::string& utf8);
void        SetFileNameFold(FileNameFoldFn fn);  // nullptr = the built-in fold
std::string BuiltinFoldFileName(const std::string& utf8);
std::string FoldFileName(const std::string& utf8);  // the active fold
bool        SameFileName(const std::string& a, const std::string& b);

// Review 13-77a L2: what a bad state says (Missing / OtherLibrary / Unreadable; empty strings for the other states):
// the library row's tooltip, the File section's red title and detail line, and "Save it again"'s tooltip.
struct LinkedFileWords {
    std::string row_tip, title, detail, save_again_tip;
};
LinkedFileWords LinkedFileWordsOf(LinkedFileState st, const std::string& file);

// Review 13-77a H1: "Save to a file..." on an unlinked library never overwrites a file of the folder that already
// holds this library's uid (another project may use it). WriteNew = the file name is a free one (FreeFileName) and the
// file is written; LinkExisting = nothing is written, the library is linked to `file` WITHOUT a common version
// (13-77b then shows every difference as changed on both sides); Refuse = linked or Attached already.
enum class SaveFileAction { WriteNew, LinkExisting, Refuse };
struct SaveFileDecision {
    SaveFileAction action = SaveFileAction::Refuse;
    std::string    file;  // LinkExisting: the folder's file
};
SaveFileDecision DecideSaveToFile(const SoundLibrary& lib, const std::vector<FolderRavlib>& folder);
// The toast of LinkExisting.
std::string LinkedExistingToastText(const std::string& file);

// Review 13-77a M1: "Save it again" (the file is gone, or the name holds another library). `target_exists_on_disk` =
// the library's own file name exists on disk right now (checked just before writing). Nothing = the file is fine again
// (a stale click); LinkExisting = the folder already holds this uid under another name (renamed in Explorer): link to
// it, write nothing; WriteSame = the name is free, write the copy there; WriteNew = write under a free name and link.
enum class RewriteAction { Nothing, WriteSame, WriteNew, LinkExisting };
struct RewriteDecision {
    RewriteAction action = RewriteAction::Nothing;
    std::string   file;  // LinkExisting: the folder's file
};
RewriteDecision DecideSaveAgain(const SoundLibrary& lib, const std::vector<FolderRavlib>& folder, bool target_exists_on_disk);
// "Locate..." picked a .ravlib that holds this library: LinkPicked (it is in the folder), LinkExisting (it is elsewhere
// but the folder already holds this uid: link to that one, copy nothing), CopyIn (elsewhere, not in the folder yet).
enum class RelinkAction { LinkPicked, LinkExisting, CopyIn };
struct RelinkDecision {
    RelinkAction action = RelinkAction::CopyIn;
    std::string  file;  // LinkPicked: the picked name; LinkExisting: the folder's file
};
RelinkDecision DecideRelink(const std::vector<FolderRavlib>& folder, const std::string& lib_uid, bool picked_in_folder,
                            const std::string& picked_name);
// "Import .ravlib...": the picked file's library is already in the project, or its file is in the folder (refused:
// Add it from the list), or it is copied in.
enum class ImportAction { AlreadyInProject, Refuse, Copy };
struct ImportDecision {
    ImportAction action = ImportAction::Copy;
    std::string  file;  // Refuse: the folder's file
};
ImportDecision DecideImport(const SoundStore& store, const std::vector<FolderRavlib>& folder, const std::string& file_uid);
std::string    ImportRefusedToastText(const std::string& file);
// Decision 2, first sight: of the linked libraries with no common version, those not seen yet (`first_time(uid)`
// is called once for each candidate) whose file is in `files` (name matched like the disk): (library uid, file text).
struct FolderRavlibText {
    std::string file, uid, text;
};
std::vector<std::pair<std::string, std::string>> FirstSightTargets(const SoundStore& store,
                                                                    const std::vector<FolderRavlibText>& files,
                                                                    const std::function<bool(const std::string&)>& first_time);

// The common version of library `lib_uid` as it is now in `store` (plain header, project uids) with the file's
// by / at / time. An unknown uid gives an empty text.
LinkBase MakeLinkBase(const SoundStore& store, const std::string& lib_uid, const std::string& by, int64_t at,
                      int64_t synced_at);
// Decision 2: a linked library with no common version whose own text equals `file_text` (the file in the folder)
// takes it as the common version, silently. False (store unchanged) when it is not linked, has one already, or
// differs.
bool AdoptFileAsBase(SoundStore& store, const std::string& lib_uid, const std::string& file_text, int64_t synced_at);

// The node's destination folder: its lands_in, else its nearest ancestor's, else kDefaultLandsIn.
// 13-77a: every library has a Lands in (an Attached one's Sounds are placed from their sources' tracks).
std::string EffectiveLandsIn(const SoundLibrary& lib, const std::string& node_uid);
// Story 13-16: the node's nearest Blend ancestor (`blend_uid`) and that Blend's child on its path
// (`child_uid`, the node itself when it is one). False (both cleared) when it has none or on a cycle.
bool BlendLayerPath(const SoundLibrary& lib, const std::string& node_uid, std::string* blend_uid,
                    std::string* child_uid);

// Where a file is now: last_path when `file_size` (-1 = absent) says it exists with the same size
// (identity size -1 = any), else the database's Found path; "" = missing (Ambiguous / none).
std::string ResolveAssetPath(const AssetIdentity& id, const std::function<int64_t(const std::string&)>& file_size,
                             const MediaDbIndex* idx);
// True when two identities are the same file: name ignoring case, size and length (+-1.5 ms) when
// both known.
bool SameAssetIdentity(const AssetIdentity& a, const AssetIdentity& b);
// Sets last_path = found path on every File Sound (every library) whose identity matches a found
// one; unknown size / length are filled from it. `picked` (optional) = the file the user picked
// (last_path = its path, size / length read from it): a Sound set to that path takes its known size /
// length, so a re-rendered file of another size is found again. Spec missing-sounds-window: an item
// Sound too when `item_ok` says so (its item gone: its remembered file is relocated, its kind kept);
// null = File Sounds only. How many Sounds changed.
int ApplyLocated(SoundStore& store, const std::vector<std::pair<AssetIdentity, std::string>>& found,
                 const AssetIdentity* picked = nullptr, const std::function<bool(const SoundNode&)>& item_ok = nullptr);
// 13-50 review: a File Sound's file was renamed and the user picked it under its new name. The Sound
// (`lib_uid` / `node_uid`) and every File Sound sharing its old file (`old_id`, same last_path ignoring
// case) take `now` whole: name, last_path, size and length. Item Sounds (the picked one included) only
// when `item_ok` says so. How many Sounds changed.
int ApplyRelinked(SoundStore& store, const std::string& lib_uid, const std::string& node_uid,
                  const AssetIdentity& old_id, const AssetIdentity& now,
                  const std::function<bool(const SoundNode&)>& item_ok = nullptr);
// Spec missing-sounds-window: a file found by the Sound's name (an older item Sound that never remembered its
// file): `found.last_path` (with its name, size, length) becomes the Sound's remembered file, its kind kept.
struct FoundByName {
    std::string   lib_uid, node_uid;
    AssetIdentity found;
};
// How many Sounds changed (a Sound that remembers a file now is left as it is).
int ApplyFoundByName(SoundStore& store, const std::vector<FoundByName>& found);

// Folder track layout (decision 12), `depths` = every track's I_FOLDERDEPTH in project order and
// `fi` a folder track (depth >= 1): fills its direct children (indices) and its last descendant
// (`fi` when none). Returns how many folders above `fi` that last descendant also closes (0 when
// it closes only `fi`). A new last child then takes depth -1 - that, and the old last descendant
// its depth + 1 + that.
int FolderChildren(const std::vector<int>& depths, int fi, std::vector<int>* kids, int* last);

// A file name for a library: \ / : * ? " < > | and control bytes -> '_', trimmed (spaces, dots),
// "" -> "Library", ".ravlib" added, " 2", " 3"... when `taken` (file names) has it, ignoring case.
std::string RavlibFileName(const std::string& lib_name, const std::vector<std::string>& taken);

// ---- 13-24: Convert a Project Library to a User Library -------------------------------------------

// One take of an Item Sound's item, as REAPER reports it (read by the caller).
struct ItemTakeFile {
    std::string type;          // its source's GetType() ("WAVE", "MIDI", "SECTION", "RPP_PROJECT", "EMPTY"...)
    std::string path;          // its file, absolute ("" = none)
    double      length = -1.0; // its source's length (s), -1 = unknown
    int64_t     size = -1;     // the file's size (bytes), -1 = unknown
};
struct ItemFileFacts {
    bool                      found = false;  // the item is in the project
    bool                      pile = false;   // a folder item (NVK pile)
    int                       active = 0;     // the active take's index
    std::vector<ItemTakeFile> takes;
    // 13-24 review: what detection reads of the item (ReadItemMedia), so the conversion freezes the
    // frames a File Sound needs (the item's trimmed portion, in source time) when `entry_ok`.
    bool                      entry_ok = false;
    EntryFacts                entry;
};
// The File source an Item Sound becomes (spec 13-24 C2, C4): kind File, the asset of the file its
// item plays (Takes mode: every take, all one file; else the active take), item_guid kept. False
// (`out` untouched) with `why` when it cannot: no item, a pile, MIDI, a subproject, a reversed /
// section take, no file, takes of different files.
bool FileSourceFromItem(const SoundData& data, const ItemFileFacts& facts, SoundSource* out, std::string* why);
// What a conversion did: the library's Sounds, how many became File Sounds, and the ones left
// project-only (node uid, name, reason) in library order.
struct ProjectOnlySound {
    std::string uid;
    std::string name;
    std::string why;
};
struct ConvertReport {
    int                           sounds = 0;
    int                           converted = 0;
    std::vector<ProjectOnlySound> project_only;
};
// Prepares a library of `store` to be saved to a file (13-77a: the link): ravlib_path = `file_name`, every Item Sound that FileSourceFromItem accepts (facts by its item GUID) becomes a
// File Sound; nothing else changes. False (store unchanged) for an unknown uid or an already linked library.
// 13-24 review: a converted Sound's frames are frozen from its item's facts (`entry_ok`): empty frames
// get the item's default detection (DefaultDetectMode without peaks, so Takes / TakeMarkers / the
// item's portion, never the whole file); unedited Whole / Takes frames get the item's portion in
// source time (its playrate).
bool ConvertLibraryToUser(SoundStore& store, const std::string& lib_uid, const std::string& file_name,
                          const std::function<ItemFileFacts(const std::string& item_guid)>& facts,
                          ConvertReport* report);
// 13-77a: Save to a file's toast: "<name> saved to <file> in the User Library folder: other projects can Add it",
// then, when any, the project-only count and up to 3 "name: reason" (13-24 C5).
std::string SaveToFileToastText(const std::string& lib_name, const std::string& file, const ConvertReport& r);
// `path` made absolute against `dir` when it is relative ("C:/p" + "Audio/x.wav"); an absolute one
// (drive letter, leading slash / backslash) unchanged.
std::string AbsoluteMediaPath(const std::string& path, const std::string& dir);

}  // namespace rav
