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
// The node's index among the children of its nearest Blend ancestor (the child on its path); 0
// when it has none.
int BlendLayerOf(const SoundLibrary& lib, const std::string& node_uid);

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

// Populate's source for a Sound (13-11): a File source whose node has a non-empty entry in
// `materialised` (node uid -> imported item GUID) becomes that Item; anything else is unchanged.
SoundSource PopulateSource(const SoundSource& src, const std::string& node_uid,
                           const std::map<std::string, std::string>* materialised);

// A file name for a library: \ / : * ? " < > | and control bytes -> '_', trimmed (spaces, dots),
// "" -> "Library", ".ravlib" added, " 2", " 3"... when `taken` (file names) has it, ignoring case.
std::string RavlibFileName(const std::string& lib_name, const std::vector<std::string>& taken);

}  // namespace rav
