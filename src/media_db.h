// SPDX-License-Identifier: MIT
//
// Media databases (13-5): REAPER's .ReaperFileList files, read in the background, and asset lookup.
// Epic 13 (D20). Created by 13-2 with declared interfaces; story 13-5 filled the bodies and added
// declarations. Pure C++ (file reads only, no REAPER call). Shared types: sound_model.h.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "sound_model.h"

namespace rav {

// One media database of REAPER (a Shortcut<N> of the Media Explorer's ini section, or the MediaDB folder).
struct DbEntry {
    std::string name;  // as shown
    std::string path;  // its .ReaperFileList file
};

// One file of a database.
struct DbFile {
    std::string path;
    int64_t     size = -1;
    double      length = -1.0;  // seconds, -1 = unknown
    int64_t     mtime = 0;
    std::string db;           // DbEntry::name
    std::string description;  // DATA d:
    std::string library;      // DATA b: (shown as "(Library)")
    int         channels = -1;  // DATA n:
    int         bits = -1;      // DATA i:
    int         rate = -1;      // DATA s:
};

enum class LookupStatus { Found, Ambiguous, Missing };

struct LookupResult {
    LookupStatus             status = LookupStatus::Missing;
    std::string              path;
    std::vector<std::string> candidates;
};

// The databases listed in the media explorer's ini text (resource_path = REAPER's): section
// [reaper_sexplorer] if present, else [reaper_explorer]; deduped. Empty when none is listed.
std::vector<DbEntry> ListMediaDatabases(const std::string& resource_path, const std::string& ini_text);
// Parses a .ReaperFileList text: files appended to `out`, `unparsed` (optional) counts the
// lines skipped. False when the text is not a file list (non-empty, no known line).
bool ParseReaperFileList(const std::string& text, const std::string& db, std::vector<DbFile>* out,
                         size_t* unparsed);
// Same, streamed; `cancel` (optional) is checked every 4096 lines (then returns false).
bool ParseReaperFileList(std::istream& in, const std::string& db, std::vector<DbFile>* out,
                         size_t* unparsed, const std::atomic<bool>* cancel = nullptr);
// A database length value ("m:ss.mmm", "h:mm:ss.mmm" or seconds) in seconds.
bool ParseDbLength(const std::string& v, double* seconds);

class MediaDbIndex {
public:
    void   Add(const DbFile& f);
    void   Clear();
    size_t Size() const;
    // By identity: name (ignoring case), size, length +-1.5 ms, then the trailing path
    // components of last_path / rel_path; mtime ignored. Unknown fields are wildcards.
    LookupResult Find(const AssetIdentity& id) const;
    // Every query word in name + description (ignoring case); db_filter "" = all databases.
    std::vector<const DbFile*> Search(const std::string& query, const std::string& db_filter, size_t max) const;
    const std::vector<DbFile>& Files() const { return files_; }

private:
    std::vector<DbFile>                                    files_;
    std::unordered_map<std::string, std::vector<uint32_t>> by_name_;  // lowercase file name -> files_
    std::vector<std::string>                               hay_;      // lowercase "name description"
};

// Finds the identity in a folder the user picked (`list_folder` lists one folder's files).
LookupResult LocateFolder(const AssetIdentity& id, const std::string& folder,
                          const std::function<std::vector<DbFile>(const std::string&)>& list_folder);
// One file was located at `located_path` (its identity `located`): maps the old folder to the new
// one by their common trailing components and looks each of `missing` up in its mapped folder.
// One result per `missing` entry (Missing when outside the old folder or not verified).
std::vector<LookupResult> RelocateFolder(const AssetIdentity& located, const std::string& located_path,
                                         const std::vector<AssetIdentity>& missing,
                                         const std::function<std::vector<DbFile>(const std::string&)>& list_folder);

// 13-38: "Locate in a folder": finds each of `missing` under `root` (the folder the user picked,
// searched with its subfolders). Phase 1, per sound: the folders root/<last k components of its old
// folder>, deepest tail first (each listed once); the first Found wins. Phase 2, for those still not
// Found: root itself, listed once; MediaDbIndex::Find ranks the candidates by their trailing
// components in common with the old path (a tie = Ambiguous, never Found). One result per `missing`
// entry. `cancel` (optional): returns early when set (the rest stays Missing).
std::vector<LookupResult> LocateUnderRoot(const std::string& root, const std::vector<AssetIdentity>& missing,
                                          const std::function<std::vector<DbFile>(const std::string&)>& list_folder,
                                          const std::atomic<bool>* cancel = nullptr);

// Fallback folder index (D7). Lists the audio files under `folder` (recursive) with their size;
// the length comes from `cached` when path + size + mtime are unchanged, else from `length_of`
// (null -> -1). `calls` (optional) counts the length_of calls. `cancel` (optional): the walk stops
// when it is set (a partial list).
std::vector<DbFile> ScanFolder(const std::string& folder, const std::vector<DbFile>& cached,
                               const std::function<double(const std::string&)>& length_of, size_t* calls,
                               const std::atomic<bool>* cancel = nullptr);
// <resource>/ReaAnimViewer/mediadb
std::string MediaCacheDir(const std::string& resource_path);
// The cache file of one folder in that dir.
std::string FolderCachePath(const std::string& resource_path, const std::string& folder);
std::string FolderCacheText(const std::vector<DbFile>& files);
bool        LoadFolderCache(const std::string& cache_path, std::vector<DbFile>* out);
bool        SaveFolderCache(const std::string& cache_path, const std::vector<DbFile>& files);
// A list_folder for LocateFolder / RelocateFolder / LocateUnderRoot: ScanFolder through the
// folder's cache file. A folder that does not exist lists nothing and writes no cache file; a
// listing cut short by `cancel` (optional, must outlive the lister) is not written either.
std::function<std::vector<DbFile>(const std::string&)> MakeCachedFolderLister(
    const std::string& resource_path, std::function<double(const std::string&)> length_of,
    const std::atomic<bool>* cancel = nullptr);

// The background load of every database (one at a time; a later call restarts it).
void MediaDbStartLoad(const std::string& resource_path);
// Cancels and joins the load (for an unload hook).
void MediaDbStop();
// True once the index of the latest load is published.
bool MediaDbReady();
// The loaded index (never null; the previous one, or empty, until ready).
std::shared_ptr<const MediaDbIndex> MediaDbGet();

struct MediaDbInfo {
    std::vector<DbEntry> databases;  // loaded, in order
    size_t               files = 0;
    size_t               unparsed = 0;
    double               seconds = 0.0;  // load time
};
MediaDbInfo MediaDbGetInfo();

// RAV's favourite media paths (files, folders accepted), one per line; deduped ignoring case.
std::vector<std::string> LoadMediaFavourites(const std::string& text);
std::string SaveMediaFavourites(const std::vector<std::string>& paths);
// <resource>/ReaAnimViewer/media_favourites.txt
std::string MediaFavouritesPath(const std::string& resource_path);
std::vector<std::string> ReadMediaFavouritesFile(const std::string& resource_path);
bool WriteMediaFavouritesFile(const std::string& resource_path, const std::vector<std::string>& paths);

}  // namespace rav
