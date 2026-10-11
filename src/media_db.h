// SPDX-License-Identifier: MIT
//
// Media databases (13-5): REAPER's .ReaperFileList files, read in the background, and asset lookup.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-5
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

// One media database of REAPER (reaper-mediadb.ini / the MediaDB folder).
struct DbEntry {
    std::string name;  // as shown
    std::string path;  // its .ReaperFileList file
};

// One file of a database.
struct DbFile {
    std::string path;
    int64_t     size = -1;
    double      length = -1.0;  // seconds
    int64_t     mtime = 0;
    std::string db;  // DbEntry::name
};

enum class LookupStatus { Found, Ambiguous, Missing };

struct LookupResult {
    LookupStatus             status = LookupStatus::Missing;
    std::string              path;
    std::vector<std::string> candidates;
};

// The databases listed in the media explorer's ini text (resource_path = REAPER's).
std::vector<DbEntry> ListMediaDatabases(const std::string& resource_path, const std::string& ini_text);
// Parses a .ReaperFileList text: files appended to `out`, `unparsed` (optional) counts the
// entries skipped. False when the text is not a file list.
bool ParseReaperFileList(const std::string& text, const std::string& db, std::vector<DbFile>* out,
                         size_t* unparsed);
// A database length value ("m:ss.mmm" or seconds) in seconds.
bool ParseDbLength(const std::string& v, double* seconds);

class MediaDbIndex {
public:
    void   Add(const DbFile& f);
    void   Clear();
    size_t Size() const;
    // By identity: name (ignoring case), size, length +-1.5 ms; paths are hints, mtime ignored.
    LookupResult Find(const AssetIdentity& id) const;
    std::vector<const DbFile*> Search(const std::string& query, const std::string& db_filter, size_t max) const;

private:
    std::vector<DbFile> files_;
};

// Finds the identity in a folder the user picked (`list_folder` lists one folder's files).
LookupResult LocateFolder(const AssetIdentity& id, const std::string& folder,
                          const std::function<std::vector<DbFile>(const std::string&)>& list_folder);

// The background load of every database (one at a time; a later call restarts it).
void MediaDbStartLoad(const std::string& resource_path);
bool MediaDbReady();
// The loaded index (never null; empty until ready).
std::shared_ptr<const MediaDbIndex> MediaDbGet();

// The Media Explorer's favourite folders (one path per line).
std::vector<std::string> LoadMediaFavourites(const std::string& text);
std::string SaveMediaFavourites(const std::vector<std::string>& folders);

}  // namespace rav
