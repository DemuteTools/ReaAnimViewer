// SPDX-License-Identifier: MIT
//
// See sound_sync.h.

#include "sound_sync.h"

#ifdef _WIN32

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "item_rules.h"        // RavTakeOf
#include "reaper_item_read.h"  // ItemGuidOf
#include "sound_place.h"       // ReadPlacedWritten
#include "sound_place_plan.h"  // PlacedAnchorOf
#include "sound_project.h"
#include "sound_record.h"
#include "sound_tracks.h"  // TrackName

namespace rav {

namespace {

struct Cache {
    bool                                  valid = false;
    int                                   state = 0;
    ReaProject*                           proj = nullptr;
    uint64_t                              gen = 0;
    std::chrono::steady_clock::time_point at;
    SyncState                             st;
};
Cache g_cache;

std::string UpperGuid(std::string g)
{
    for (char& c : g)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return g;
}

SyncState Scan()
{
    const SoundStore& store = GetSoundStore(nullptr);
    if (store.libraries.empty()) return SyncState{};
    std::vector<SyncOwner>        owners;
    std::map<std::string, size_t> owner_at;  // GUID (upper case) -> owners index
    std::vector<MediaItem*>       owner_items;
    std::vector<std::pair<MediaItem*, PlacedTag>> placed;
    const int                     n = CountMediaItems(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(nullptr, i);
        if (!it) continue;
        PlacedTag tag;
        if (ReadPlacedTag(it, &tag)) {
            if (tag.own == Ownership::Populated && !tag.lib.empty()) placed.push_back({it, tag});
            continue;
        }
        MediaItem_Take* take = RavTakeOf(it);
        if (!take) continue;
        SyncOwner o;
        o.guid = ItemGuidOf(it);
        if (o.guid.empty()) continue;
        if (const char* nm = GetTakeName(take)) o.name = nm;
        if (MediaTrack* tr = GetMediaItem_Track(it)) o.track = TrackName(tr);
        o.selected = GetMediaItemInfo_Value(it, "B_UISEL") != 0.0;
        owner_at[UpperGuid(o.guid)] = owners.size();
        owners.push_back(o);
        owner_items.push_back(it);
    }
    std::map<std::string, ItemSounds> records;  // owner GUID (upper case) -> its record, read once
    std::vector<SyncPlacedItem>       items;
    for (const auto& pt : placed) {
        MediaItem*       it = pt.first;
        const PlacedTag& tag = pt.second;
        SyncPlacedItem   x;
        x.tag = tag;
        x.owner = tag.owner;
        x.guid = ItemGuidOf(it);
        // Only the ones behind are read further (the card counts the libraries they play from the tags alone).
        if (BehindNodes(tag, store).empty()) {
            items.push_back(x);
            continue;
        }
        if (MediaItem_Take* take = GetActiveTake(it))
            if (const char* nm = GetTakeName(take)) x.name = nm;
        x.now = ReadPlacedWritten(it);
        double sync = 0.0;
        PlacedAnchorOf(tag, GetMediaItemInfo_Value(it, "D_POSITION"), GetMediaItemInfo_Value(it, "D_SNAPOFFSET"), &x.anchor, &sync);
        const std::string ok = UpperGuid(tag.owner);
        const auto        oh = owner_at.find(ok);
        if (oh == owner_at.end()) continue;  // its animation item is gone: nothing to sync it from
        {
            auto rh = records.find(ok);
            if (rh == records.end()) rh = records.emplace(ok, ReadItemSounds(owner_items[oh->second])).first;
            const std::string g = UpperGuid(x.guid);
            for (const EventSounds& e : rh->second.events)
                for (const SoundPlacement& p : e.placed)
                    if (UpperGuid(p.item_guid) == g) x.nudge_s = p.offset_s;
            x.owner = owners[oh->second].guid;
        }
        items.push_back(x);
    }
    return BuildSyncState(store, items, owners);
}

}  // namespace

const SyncState& PlacedSyncState()
{
    ReaProject*    proj = nullptr;
    int            state = 0;
    uint64_t       gen = 0;
    const auto     now = std::chrono::steady_clock::now();
    bool           scan = false;
    try {
        proj = EnumProjects(-1, nullptr, 0);
        state = GetProjectStateChangeCount(nullptr);
        gen = SoundStoreGeneration();
        const bool moved = !g_cache.valid || proj != g_cache.proj || state != g_cache.state || gen != g_cache.gen;
        scan = moved && (!g_cache.valid || now - g_cache.at >= std::chrono::milliseconds(250));
        if (scan) g_cache.st = Scan();
    } catch (...) {
        g_cache.st = SyncState{};  // left empty until the project or the store moves again (never retried each frame)
    }
    if (scan) {
        g_cache.valid = true;
        g_cache.proj = proj;
        g_cache.state = state;
        g_cache.gen = gen;
        g_cache.at = now;
    }
    return g_cache.st;
}

void InvalidatePlacedSyncState() { g_cache.valid = false; }

}  // namespace rav

#endif  // _WIN32
