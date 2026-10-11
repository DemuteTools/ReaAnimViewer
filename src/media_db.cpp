// SPDX-License-Identifier: MIT
//
// See media_db.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-5.

#include "media_db.h"


namespace rav {

std::vector<DbEntry> ListMediaDatabases(const std::string&, const std::string&)
{
    return {};  // STUB (13-5)
}

bool ParseReaperFileList(const std::string&, const std::string&, std::vector<DbFile>*, size_t* unparsed)
{
    if (unparsed) *unparsed = 0;
    return false;  // STUB (13-5)
}

bool ParseDbLength(const std::string&, double*)
{
    return false;  // STUB (13-5)
}

void MediaDbIndex::Add(const DbFile&)
{
    // STUB (13-5)
}

void MediaDbIndex::Clear()
{
    files_.clear();  // STUB (13-5)
}

size_t MediaDbIndex::Size() const
{
    return files_.size();  // STUB (13-5)
}

LookupResult MediaDbIndex::Find(const AssetIdentity&) const
{
    return {};  // STUB (13-5): Missing
}

std::vector<const DbFile*> MediaDbIndex::Search(const std::string&, const std::string&, size_t) const
{
    return {};  // STUB (13-5)
}

LookupResult LocateFolder(const AssetIdentity&, const std::string&,
                          const std::function<std::vector<DbFile>(const std::string&)>&)
{
    return {};  // STUB (13-5): Missing
}

void MediaDbStartLoad(const std::string&)
{
    // STUB (13-5)
}

bool MediaDbReady()
{
    return false;  // STUB (13-5)
}

std::shared_ptr<const MediaDbIndex> MediaDbGet()
{
    static const std::shared_ptr<const MediaDbIndex> empty = std::make_shared<MediaDbIndex>();
    return empty;  // STUB (13-5)
}

std::vector<std::string> LoadMediaFavourites(const std::string&)
{
    return {};  // STUB (13-5)
}

std::string SaveMediaFavourites(const std::vector<std::string>&)
{
    return {};  // STUB (13-5)
}

}  // namespace rav
