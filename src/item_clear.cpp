// SPDX-License-Identifier: MIT
//
// See item_clear.h.

#include "item_clear.h"

#ifdef _WIN32

#include <cstring>
#include <map>

#include "item_rules.h"
#include "reaper_item_read.h"  // ItemGuidOf (13-51)
#include "sound_model.h"
#include "sound_place.h"
#include "sound_project.h"
#include "tag_markers.h"

namespace rav {
namespace {

constexpr char kUndoClear[] = "RAV: Clear item data";

std::string Upper(const std::string& s)
{
    std::string u = s;
    for (char& c : u)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return u;
}

// A take key's text ("" when none). The records are at most 1 MB (item_rules.h, sound_project.h).
std::string TakeText(MediaItem_Take* take, const char* key)
{
    static std::vector<char> buf;
    const size_t             size = (kItemRulesMaxBytes > kItemSoundsMaxBytes ? kItemRulesMaxBytes : kItemSoundsMaxBytes) + 1;
    if (buf.size() != size) buf.assign(size, '\0');
    buf[0] = '\0';
    if (!take || !GetSetMediaItemTakeInfo_String(take, key, buf.data(), false)) return {};
    buf[size - 1] = '\0';
    return buf.data();
}

// An item key's text ("" when none). A RAV_SND tag is one short line.
std::string ItemText(MediaItem* item, const char* key)
{
    char buf[4096] = {};
    if (!GetSetMediaItemInfo_String(item, key, buf, false)) return {};
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

// Writes a take key ("" removes it). No undo point (inside the gesture's). Judged by reading the
// key back: REAPER can answer false to a removal (an empty value) that it did make.
bool SetTakeText(MediaItem_Take* take, const char* key, const std::string& text)
{
    if (!take || !ValidatePtr2(nullptr, take, "MediaItem_Take*")) return false;
    GetSetMediaItemTakeInfo_String(take, key, const_cast<char*>(text.c_str()), true);
    return TakeText(take, key) == text;
}

// Everything the plan reads, and the handles it acts on.
struct Gathered {
    std::vector<MediaItem*>           items;    // the targets (the item first)
    std::vector<MediaItem_Take*>      takes;
    std::vector<ClearTarget>          targets;
    std::map<std::string, MediaItem*> tagged_items;  // upper GUID -> item
    ItemClearPlan                     plan;
};

bool Gather(MediaItem* item, Gathered* g)
{
    if (!item || !ValidatePtr2(nullptr, item, "MediaItem*") || !RavTakeOf(item)) return false;
    for (MediaItem* it : WithPoolMembers({item})) {
        MediaItem_Take* take = RavTakeOf(it);
        if (!take) continue;
        ClearTarget t;
        t.guid = ItemGuidOf(it);  // reaper_item_read.h
        t.rules_raw = TakeText(take, kItemRulesKey);
        t.committed_raw = TakeText(take, kItemRulesCommittedKey);
        t.sounds_raw = TakeText(take, kItemSoundsKey);
        g->items.push_back(it);
        g->takes.push_back(take);
        g->targets.push_back(std::move(t));
    }
    if (g->items.empty()) return false;
    // Every tagged item of the project (one scan; the tag read straight, no live check per item).
    std::vector<ClearTagged> tagged;
    const int                n = CountMediaItems(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(nullptr, i);
        if (!it) continue;
        std::string tag = ItemText(it, kPlacedTagKey);
        if (tag.empty()) continue;
        ClearTagged t;
        t.guid = ItemGuidOf(it);  // reaper_item_read.h
        if (t.guid.empty()) continue;
        t.tag_raw = std::move(tag);
        g->tagged_items[Upper(t.guid)] = it;
        tagged.push_back(std::move(t));
    }
    // The Project Library Sounds' source items: never deleted.
    g->plan = PlanItemClear(g->targets, tagged, LibrarySourceGuids(GetSoundStore(nullptr)));
    return true;
}

}  // namespace

ItemClearInfo ReadItemClear(MediaItem* item)
{
    ItemClearInfo info;
    try {
        Gathered g;
        if (!Gather(item, &g)) return info;
        info.anything = g.plan.anything;
        info.counts = g.plan.counts;
    } catch (...) {
        info = ItemClearInfo();
    }
    return info;
}

ItemClearResult ClearItemData(MediaItem* item)
{
    ItemClearResult res;
    try {
        Gathered g;
        if (!Gather(item, &g) || !g.plan.anything) return res;
        res.counts = g.plan.counts;
        res.items = g.items;
        res.counts.sounds = 0;
        res.counts.attached = 0;
        {
            SoundUndoScope undo(nullptr, kUndoClear);
            PlaceBatch(true);  // one redraw, the gesture's live-item cache (LiveItemsBatch)
            struct BatchEnd {
                ~BatchEnd() { PlaceBatch(false); }
            } batch_end;
            res.ran = true;
            // 1. The markers RAV owns, while the records still list them (10-11: a project marker
            // another item wants is handed over to it, `handoffs`).
            std::vector<MarkerHandOff> handoffs;
            for (size_t i = 0; i < g.items.size(); ++i)
                if (g.plan.markers[i]) DeleteItemMarkersNoUndo(g.items[i], g.items, &handoffs);
            // 2. The records. A failed write stops the clear (the undo block still closes).
            for (const ClearRecordWrite& w : g.plan.records) {
                MediaItem_Take* take = g.takes[w.target];
                bool            ok = true;
                if (w.rules) ok = ok && SetTakeText(take, kItemRulesKey, w.rules_text);
                if (w.committed) ok = ok && SetTakeText(take, kItemRulesCommittedKey, std::string());
                if (w.sounds) ok = ok && SetTakeText(take, kItemSoundsKey, std::string());
                if (!ok) {
                    res.error = "REAPER refused to clear an item's data. Ctrl+Z undoes what was cleared.";
                    break;
                }
            }
            // A record not cleared still lists what it handed over: taken back (one owner per marker).
            if (!res.error.empty()) RevertFailedHandOversNoUndo(handoffs);
            if (res.error.empty()) {
                // 3. The sounds: library sources untagged (they stay), the others deleted.
                for (const std::string& guid : g.plan.untag_guids) {
                    const auto f = g.tagged_items.find(Upper(guid));
                    if (f != g.tagged_items.end()) ClearPlacedTag(f->second);
                }
                for (const std::string& guid : g.plan.delete_guids) {
                    const auto f = g.tagged_items.find(Upper(guid));
                    if (f == g.tagged_items.end()) continue;
                    PlacedTag  tag;
                    const bool attached = ReadPlacedTag(f->second, &tag) && tag.own == Ownership::Attached;
                    if (RemovePlacedAny(nullptr, f->second)) {  // it leaves the live cache first
                        ++res.counts.sounds;
                        if (attached) ++res.counts.attached;
                    }
                }
            }
            MarkProjectDirty(nullptr);
        }
        UpdateArrange();
    } catch (...) {
        if (res.error.empty()) res.error = "The item's data could not be cleared.";
    }
    return res;
}

}  // namespace rav

#endif  // _WIN32
