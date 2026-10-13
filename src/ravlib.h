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
constexpr const char kRavlibExt[] = ".ravlib";
// Where a User Library Sound lands when no node above it says (decision 12).
constexpr const char kDefaultLandsIn[] = "RAV Library";

// Reads a .ravlib into `out` (one library, its groups). False (and `out` untouched) when it is not one:
// no "RAVLIB <n>" (n >= 1) first line, or not exactly one library.
bool ParseRavlib(const std::string& text, SoundStore* out);
// Writes one library and the groups its Switches use (pass RavlibOf's result).
std::string SerializeRavlib(const SoundStore& one);

// The export form of one library of `store`: the library (ravlib_path cleared, File sources'
// item_guid cleared) and the groups its Switches use, in store order. `item_sounds` (optional)
// gets how many of its Sounds come from project items (missing anywhere else). An unknown uid
// gives an empty store.
SoundStore RavlibOf(const SoundStore& store, const std::string& lib_uid, int* item_sounds);

// Import merge (decision 7). False and `store` unchanged when `file` is not one library or its
// uid is already in the store (`why` "Already in this project", `lib_uid` = that uid).
bool AddRavlibLibrary(SoundStore& store, const SoundStore& file, const std::string& file_name, std::string* lib_uid,
                      std::string* why);

// The node's destination folder: its lands_in, else its nearest ancestor's, else kDefaultLandsIn.
// A library that is not a User Library always gives kDefaultLandsIn.
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
// length, so a re-rendered file of another size is found again. How many Sounds changed.
int ApplyLocated(SoundStore& store, const std::vector<std::pair<AssetIdentity, std::string>>& found,
                 const AssetIdentity* picked = nullptr);

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
// Turns a Project Library of `store` into a User Library in place: user = true, ravlib_path =
// `file_name`, every Item Sound that FileSourceFromItem accepts (facts by its item GUID) becomes a
// File Sound; nothing else changes. False (store unchanged) for an unknown uid or a User Library.
// 13-24 review: a converted Sound's frames are frozen from its item's facts (`entry_ok`): empty frames
// get the item's default detection (DefaultDetectMode without peaks, so Takes / TakeMarkers / the
// item's portion, never the whole file); unedited Whole / Takes frames get the item's portion in
// source time (its playrate).
bool ConvertLibraryToUser(SoundStore& store, const std::string& lib_uid, const std::string& file_name,
                          const std::function<ItemFileFacts(const std::string& item_guid)>& facts,
                          ConvertReport* report);
// The conversion's toast: "Converted to a User Library (<file>)", then the project-only count and up
// to 3 "name: reason" (C5).
std::string ConvertReportText(const ConvertReport& r, const std::string& file_name);
// `path` made absolute against `dir` when it is relative ("C:/p" + "Audio/x.wav"); an absolute one
// (drive letter, leading slash / backslash) unchanged.
std::string AbsoluteMediaPath(const std::string& path, const std::string& dir);

}  // namespace rav
