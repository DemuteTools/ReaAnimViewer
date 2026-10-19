// SPDX-License-Identifier: MIT
//
// See sound_spread_selection.h (13-68).

#include "sound_spread_selection.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <map>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include <windows.h>  // 13-68e v5: GetAsyncKeyState, GetCursorPos (read only)

#include "auto_detect.h"  // IsActiveAutoBlock, AutoBlockKind
#include "auto_types.h"   // AutoKindRow
#include "bone_events.h"  // IsAutoBlock
#include "busy_progress_ui.h"
#include "console_log.h"
#include "event_list.h"
#include "item_rules.h"
#include "reaper_api.h"
#include "reaper_item_read.h"
#include "reaper_toast.h"
#include "rule_record.h"
#include "sound_assign.h"  // EventSide
#include "sound_attach.h"
#include "sound_attach_plan.h"
#include "sound_follow.h"       // 13-68e v5: LocalizedUndoNames, FollowCutNames
#include "sound_auto_assign.h"  // SetRulesLibrary, SetRulesValue
#include "sound_peaks.h"        // ReadItemMedia
#include "sound_place.h"        // FolderPileMembers
#include "sound_populate.h"
#include "sound_populate_plan.h"  // SetValueIn
#include "sound_project.h"
#include "sound_record.h"
#include "sound_spread_plan.h"
#include "tag_markers.h"
#include "tagging_session.h"

namespace rav {
namespace {

using SetMediaItemInfo_ValueFn = bool (*)(MediaItem* item, const char* parmname, double newvalue);

using CountTakesFn = int (*)(MediaItem* item);

SetMediaItemInfo_ValueFn f_set_item = nullptr;
CountTakesFn             f_count_takes = nullptr;
// 13-68e v5 (the watcher): a redo waiting (an undo just ran).
using Undo_CanRedo2Fn = const char* (*)(ReaProject* proj);
Undo_CanRedo2Fn    f_can_redo = nullptr;
// An original deleted by hand put back (its last read state).
using GetItemStateChunkFn = bool (*)(MediaItem* item, char* buf, int buf_sz, bool isundo);
using SetItemStateChunkFn = bool (*)(MediaItem* item, const char* str, bool isundo);
using AddMediaItemToTrackFn = MediaItem* (*)(MediaTrack* tr);
GetItemStateChunkFn   f_get_chunk = nullptr;
SetItemStateChunkFn   f_set_chunk = nullptr;
AddMediaItemToTrackFn f_add_item = nullptr;

constexpr const char kNoMarker[] = "Select a RAV marker (click its label)";
constexpr const char kTakeOnly[] = "RAV markers are take markers only: set Markers to Project or Both";

// A GUID compared without case (SameGuid).
std::string GuidKey(std::string g)
{
    for (char& c : g) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return g;
}

// The RAV marker selected in REAPER (the first in time) and how many are selected.
struct SelectedMarker {
    MediaItem*  item = nullptr;
    std::string name;  // as written (a preview's carries its suffix)
    double      pos = 0.0;
};
int FindSelectedMarker(const std::vector<MediaItem*>& rav_items, SelectedMarker* out)
{
    std::set<std::string> seen;
    int                   count = 0;
    bool                  owner_found = false;
    for (MediaItem* it : rav_items) {
        ItemRulesRead rr;
        if (!ReadItemRules(it, &rr) || !rr.valid) continue;
        const bool owner = SameGuid(RecordOwner(rr.rules), ItemGuidOf(it));
        for (const std::vector<ProjectMarkerRef>* refs : {&rr.rules.pmarkers, &rr.rules.ppmarkers})
            for (const ProjectMarkerRef& r : *refs) {
                double pos = 0.0;
                if (r.guid.empty() || !ProjectMarkerSelected(r.guid, &pos)) continue;
                const bool fresh = seen.insert(r.guid).second;
                if (fresh) ++count;
                // The first in time; a marker listed by two items (a copy's record): its owner's.
                const bool same = out->item && std::fabs(pos - out->pos) < 1e-9 && out->name == r.name;
                if (!out->item || pos < out->pos - 1e-9 || (same && owner && !owner_found)) {
                    out->item = it;
                    out->name = r.name;
                    out->pos = pos;
                    owner_found = owner;
                }
            }
    }
    return count;
}

// 13-68 v3 (both sides by default): a rule's marker kind with its sides, from the rule (an auto block's kind and its
// side-free marker word, else the rule's marker: a user rule has no side), never from a marker's name.
SpreadKind KindOfBlock(const Block& b)
{
    AutoKind k;
    char     side = 0;
    if (IsAutoBlock(b) && AutoBlockKind(b, &k, &side)) {
        const char* word = AutoKindRow(AutoKindRow(k).sound_key).marker;
        return SpreadKindOf(AutoSoundKeyOf(b), word ? word : "", b.marker);
    }
    return SpreadKindOf(std::string(), std::string(), b.marker);
}

// The blocks of `rules` of the kind `key` (SpreadKind::key, both sides) that are on.
std::vector<char> BlocksOfType(const ItemRules& rules, const std::string& key)
{
    std::vector<char> out(rules.blocks.size(), 0);
    for (size_t b = 0; b < rules.blocks.size(); ++b)
        out[b] = rules.blocks[b].enabled && KindOfBlock(rules.blocks[b]).key == key ? 1 : 0;
    return out;
}

// The kind of a marker named `name` on an item with these rules (its rule's, RuleOfMarker), and that rule's index.
SpreadKind KindOfRuleMarker(const ItemRules& rules, const std::string& name, int* block)
{
    std::vector<std::string> markers;
    for (const Block& b : rules.blocks) markers.push_back(b.marker);
    *block = RuleOfMarker(markers, name);
    return *block >= 0 ? KindOfBlock(rules.blocks[static_cast<size_t>(*block)]) : SpreadKind();
}

// The side-free name of the kind `key` on `anim` (its first rule of that kind); `fallback` when none.
std::string KindNameOn(MediaItem* anim, const std::string& key, const std::string& fallback)
{
    ItemRulesRead rr;
    if (ReadItemRules(anim, &rr) && rr.valid)
        for (const Block& b : rr.rules.blocks) {
            const SpreadKind k = KindOfBlock(b);
            if (k.key == key) return k.name;
        }
    return fallback;
}

// The markers of the kind on `item` (DetectItem -> BuildEventList -> AttachEventsOf). False when the
// item cannot be read.
bool MarkersOfType(MediaItem* item, const std::string& type, std::vector<AttachEvent>* out)
{
    const ItemDetection det = DetectItem(item);
    if (det.status != ItemDetection::Status::Ok) return false;
    const std::vector<char>       on = BlocksOfType(det.rules, type);
    const std::vector<ShownEvent> shown = BuildEventList(det.events, det.rules.events, det.rules.blocks.size());
    for (const AttachEvent& e : AttachEventsOf(shown, det.map))
        if (e.block >= 0 && static_cast<size_t>(e.block) < on.size() && on[static_cast<size_t>(e.block)]) out->push_back(e);
    return true;
}

// The variation `item` plays of its Sound `d`: its current take (Takes), else the variation starting nearest its
// take's start in the source.
int PlayedVariation(const SoundData& d, MediaItem* item)
{
    if (d.frames.empty() || !item) return -1;
    if (d.mode == DetectMode::Takes) {
        const int k = static_cast<int>(GetMediaItemInfo_Value(item, "I_CURTAKE"));
        return k >= 0 && k < static_cast<int>(d.frames.size()) ? k : -1;
    }
    MediaItem_Take* take = GetActiveTake(item);
    const double    offs = take ? GetMediaItemTakeInfo_Value(take, "D_STARTOFFS") : 0.0;
    int             best = 0;
    for (size_t k = 1; k < d.frames.size(); ++k)
        if (std::fabs(d.frames[k].start - offs) < std::fabs(d.frames[static_cast<size_t>(best)].start - offs)) best = static_cast<int>(k);
    return best;
}

std::string Finish(const std::string& text)
{
    LogInfo("Spread selection: %s", text.c_str());
    ShowReaperToast(text);
    return text;
}

// The ids of the rules of `anim` writing the type (the rules given ids first, inside the caller's undo scope).
// 13-68e: `side` ('L' / 'R') = only that side's rules (EventSide); 0 = both.
std::vector<std::string> TypeRids(MediaItem* anim, const std::string& type, char side = 0)
{
    std::vector<std::string> rids, out;
    std::string              err;
    if (!ItemRuleIds(anim, &rids, &err)) {
        LogWarn("Spread selection: %s", err.c_str());
        return out;
    }
    ItemRulesRead rr;
    if (!ReadItemRules(anim, &rr) || !rr.valid) return out;
    const std::vector<char> on = BlocksOfType(rr.rules, type);
    for (size_t b = 0; b < on.size() && b < rids.size(); ++b)
        if (on[b] && !rids[b].empty() && (!side || EventSide(rr.rules.blocks[b]) == side)) out.push_back(rids[b]);
    return out;
}

// 13-68e: the rids of the kind's rules of `side` on these items and their linked copies (read, never written).
std::set<std::string> SideRids(const std::vector<MediaItem*>& items, const std::string& type, char side)
{
    std::set<std::string> out;
    for (MediaItem* it : WithPoolMembers(items)) {
        ItemRulesRead rr;
        if (!ReadItemRules(it, &rr) || !rr.valid) continue;
        const std::vector<char> on = BlocksOfType(rr.rules, type);
        for (size_t b = 0; b < on.size(); ++b)
            if (on[b] && !rr.rules.blocks[b].rid.empty() && EventSide(rr.rules.blocks[b]) == side) out.insert(rr.rules.blocks[b].rid);
    }
    return out;
}

// 13-68e: a library with a RAV - Side Switch: every rule holding it (any RAV item) gets its side's value at rule level
// when it has none (FillMissingSideValues; Populate does the same before it places), so each event plays its side.
void ApplySideValues(ReaProject* proj, const std::string& lib_uid)
{
    const SoundStore    st = GetSoundStore(proj);
    const SoundLibrary* lib = FindLibrary(st, lib_uid);
    if (!lib || !LibraryUsesSide(st, *lib)) return;
    for (MediaItem* it : ProjectRavItems()) {
        ItemRulesRead rr;
        if (!ReadItemRules(it, &rr) || !rr.valid) continue;
        std::vector<RuleSide> sides;
        for (const Block& b : rr.rules.blocks)
            if (!b.rid.empty()) sides.push_back({b.rid, EventSide(b)});
        ModifyItemSounds(it, nullptr, [&](ItemSounds& x) { return FillMissingSideValues(st, x, sides); });
    }
}


// The selection read as the library reads it (an NVK folder item: its pile, its layers' takes shared), and the
// items each pile moves: `first` = the pile that is not copied (it is attached to an event).
struct ReadSounds {
    std::vector<SpreadSource> sources;
    std::vector<MediaItem*>   to_move;       // the originals and the folder piles' child items
    std::set<MediaItem*>      first_items;   // the first pile's (and its folder pile's children)
    std::vector<MediaItem*>   attach_items;  // those of them that sound (a folder item's children, not it)
    // 13-68 v3: the first pile's folder items (an NVK pile): the folder's GUID (its library Sound's source) and its
    // children, the top one first (its variation is the pile's).
    std::vector<std::pair<std::string, std::vector<MediaItem*>>> first_folders;
};
ReadSounds ReadSources(const std::vector<SelectedSound>& sel, const std::vector<int>& first)
{
    ReadSounds           r;
    std::set<MediaItem*> moving;
    for (size_t si = 0; si < sel.size(); ++si) {
        const SelectedSound& s = sel[si];
        const bool in_first = std::find(first.begin(), first.end(), static_cast<int>(si)) != first.end();
        SpreadSource src;
        src.facts = s.facts;
        src.name = ItemSoundName(s.item);
        src.folder = s.folder;
        if (s.folder) {
            const std::vector<MediaItem*> kids = FolderPileMembers(s.item);
            int                           takes = 0;
            for (MediaItem* k : kids) {
                takes = std::max(takes, f_count_takes ? f_count_takes(k) : 1);
                if (moving.insert(k).second) r.to_move.push_back(k);
                if (in_first && r.first_items.insert(k).second) r.attach_items.push_back(k);
            }
            if (in_first && !kids.empty()) r.first_folders.push_back({s.facts.guid, kids});
            SoundMedia top_media;  // take markers (NVK): the top member's slices are the pile's variations
            if (!kids.empty()) ReadItemMedia(kids.front(), &top_media);
            src.sound = PileSoundData(s.facts.guid, takes, s.facts.len, top_media.facts.slices);
        } else {
            src.sound = ItemSoundData(s.item);
            if (MediaItem_Take* take = GetActiveTake(s.item)) src.playrate = GetMediaItemTakeInfo_Value(take, "D_PLAYRATE");
        }
        if (moving.insert(s.item).second) r.to_move.push_back(s.item);
        if (in_first && r.first_items.insert(s.item).second && !s.folder) r.attach_items.push_back(s.item);
        r.sources.push_back(src);
    }
    return r;
}

// The selected items become the library's sources: one RAV placed or attached leaves its event (its owner's record),
// none keeps a RAV tag (Antho 2026-10-18: any selected sound is taken, whatever placed it).
void LeaveOldPlaces(ReaProject* proj, const std::vector<SelectedSound>& sel)
{
    for (const SelectedSound& s : sel) {
        if (!s.tagged) continue;
        if (MediaItem* owner = ScanItemByGuid(proj, s.tag.owner)) {
            const std::string g = s.facts.guid;
            ModifyItemSounds(owner, nullptr, [&g](ItemSounds& r) { return DetachFromRecord(r, g); });
        }
        ClearPlacedTag(s.item);
    }
}

// The items of `to_move` not in `keep` go to the source zone: after the project end, their relative positions kept.
void ToSourceZone(ReaProject* proj, const std::vector<MediaItem*>& to_move, const std::set<MediaItem*>& keep)
{
    std::set<std::string> moving;
    for (MediaItem* it : to_move)
        if (!keep.count(it)) moving.insert(GuidKey(ItemGuidOf(it)));
    double project_end = 0.0;
    for (int i = 0, n = CountMediaItems(proj); i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (!it || moving.count(GuidKey(ItemGuidOf(it)))) continue;
        project_end = std::max(project_end, GetMediaItemInfo_Value(it, "D_POSITION") + GetMediaItemInfo_Value(it, "D_LENGTH"));
    }
    std::vector<MediaItem*> zone;
    std::vector<double>     from;
    for (MediaItem* it : to_move)
        if (!keep.count(it)) {
            zone.push_back(it);
            from.push_back(GetMediaItemInfo_Value(it, "D_POSITION"));
        }
    const std::vector<double> to = SourceZonePositions(project_end, from);
    for (size_t i = 0; i < zone.size() && i < to.size(); ++i) f_set_item(zone[i], "D_POSITION", to[i]);
}

// `items` attached to the event `ev` (its rid set) of `owner` at `offset_s` from it (13-10's record and tag): they
// follow that event and Populate leaves it alone; what they play is recorded as the event's draws, so the copies'
// avoid-repeat counts it. 13-68 v3: a folder pile of `folders` (its children among `items`) records its folder's
// Sound (the library's source) at the variation its top child plays (its take, its slice).
void AttachToEvent(ReaProject* proj, MediaItem* owner, const AttachEvent& ev, const std::string& lib_uid,
                   const std::vector<MediaItem*>& items, double offset_s,
                   const std::vector<std::pair<std::string, std::vector<MediaItem*>>>& folders = {})
{
    // 13-68e v5 (Item scope): no library (nothing attached on the kind yet): the items are attached all the same.
    static const SoundLibrary kNoLibrary;
    const SoundLibrary*       lib = lib_uid.empty() ? nullptr : FindLibrary(GetSoundStore(proj), lib_uid);
    if (!lib) lib = &kNoLibrary;
    if (ev.rid.empty()) return;
    std::vector<SoundPlacement>              placements;
    std::vector<std::pair<std::string, int>> played;
    std::map<MediaItem*, int>                kid_variation;  // a folder pile's children: the pile's variation
    for (const auto& fp : folders) {
        int v = -1;
        for (const SoundNode& n : lib->nodes)
            if (n.type == SoundNodeType::Sound && SameGuid(n.sound.source.item_guid, fp.first) && !fp.second.empty())
                v = PlayedVariation(n.sound, fp.second.front());
        played.push_back({fp.first, v});
        for (MediaItem* k : fp.second) kid_variation[k] = v;
    }
    for (MediaItem* it : items) {
        ItemFacts f;
        if (!ReadItemFacts(it, &f)) continue;
        int        v = -1;
        const auto kid = kid_variation.find(it);
        if (kid != kid_variation.end()) v = kid->second;
        else {
            for (const SoundNode& n : lib->nodes)
                if (n.type == SoundNodeType::Sound && SameGuid(n.sound.source.item_guid, f.guid)) v = PlayedVariation(n.sound, it);
            played.push_back({f.guid, v});
        }
        SoundPlacement p;
        p.item_guid = f.guid;
        p.own = Ownership::Attached;
        p.offset_s = offset_s;
        p.track_guid = f.track_guid;
        p.variation = v;
        placements.push_back(p);
    }
    const std::vector<SoundDraw> draws = PlayedDraws(*lib, played);
    ModifyItemSounds(owner, nullptr, [&](ItemSounds& rec) {
        for (const SoundPlacement& p : placements) AddToRecord(rec, ev.rid, ev.clip_t, p);
        if (EventSounds* e = FindEventSounds(rec, ev.rid, ev.clip_t))
            for (const SoundDraw& d : draws) {
                auto same = std::find_if(e->draws.begin(), e->draws.end(), [&d](const SoundDraw& x) { return x.node == d.node; });
                if (same != e->draws.end()) *same = d;
                else e->draws.push_back(d);
            }
        return true;
    });
    const std::string owner_guid = ItemGuidOf(owner);
    for (MediaItem* it : items) WritePlacedTag(it, AttachTagOf(owner_guid, ev));
}

// The rid of `ev`'s rule on `owner` (the rules given ids first). "" when it has none.
std::string EventRid(MediaItem* owner, const AttachEvent& ev)
{
    std::vector<std::string> rids;
    std::string              err;
    if (!ItemRuleIds(owner, &rids, &err) || ev.block < 0 || static_cast<size_t>(ev.block) >= rids.size()) {
        LogWarn("Spread selection: no rule id for the event (%s)", err.c_str());
        return std::string();
    }
    return rids[static_cast<size_t>(ev.block)];
}

// `targets` plus every other RAV item the library is assigned to (an earlier run's).
std::vector<MediaItem*> LibraryTargets(std::vector<MediaItem*> targets, const std::string& lib_uid)
{
    for (MediaItem* it : ProjectRavItems()) {
        if (std::find(targets.begin(), targets.end(), it) != targets.end()) continue;
        for (const RuleSounds& r : ReadItemSounds(it).rules)
            if (r.library == lib_uid && !r.none) {
                targets.push_back(it);
                break;
            }
    }
    return targets;
}

// The Spread / Attached sounds already on the events of `lib_uid`'s rules of `targets` (`only` non-null: those rules)
// leave them, `moving` (the run's own originals) excepted. Returns how many went to the source zone.
int DropEarlierSounds(ReaProject* proj, const std::vector<MediaItem*>& targets, const std::string& lib_uid,
                      const std::set<std::string>* only, const std::vector<MediaItem*>& moving)
{
    int n_sounds = 0;
    // The Spread / Attached sounds already on the events of the library's rules (an earlier spread or attach, or
    // attached by hand) leave them: Populate keeps an event holding one, so it would draw nothing new there. 13-68 v3:
    // never deleted: detached from the record, untagged and moved to the source zone (they may be sources).
    {
        std::set<std::string> moving_guids;
        for (MediaItem* it : moving) moving_guids.insert(GuidKey(ItemGuidOf(it)));
        std::map<std::string, MediaItem*> by_guid;
        for (int i = 0, n = CountMediaItems(proj); i < n; ++i)
            if (MediaItem* it = GetMediaItem(proj, i)) by_guid[GuidKey(ItemGuidOf(it))] = it;
        std::vector<MediaItem*> earlier;
        for (MediaItem* m : WithPoolMembers(targets)) {
            ItemSounds            rec = ReadItemSounds(m);
            std::set<std::string> rids;
            for (const RuleSounds& r : rec.rules)
                if (r.library == lib_uid && !r.none && (!only || only->count(r.rid))) rids.insert(r.rid);
            if (rids.empty()) continue;
            std::vector<std::string> drop;
            for (const EventSounds& e : rec.events)
                if (rids.count(e.rid))
                    for (const SoundPlacement& p : e.placed)
                        if (p.own == Ownership::Attached || p.own == Ownership::Spread) drop.push_back(p.item_guid);
            if (drop.empty()) continue;
            for (const std::string& g : drop) {
                const std::string key = GuidKey(g);
                if (moving_guids.count(key)) continue;
                auto hit = by_guid.find(key);
                if (hit == by_guid.end()) continue;
                ClearPlacedTag(hit->second);
                earlier.push_back(hit->second);
                by_guid.erase(hit);
            }
            ModifyItemSounds(m, nullptr, [&drop](ItemSounds& r) {
                bool any = false;
                for (const std::string& g : drop) any = DetachFromRecord(r, g) || any;
                return any;
            });
        }
        // 13-68 v3 review: an earlier child of a folder pile (NVK) takes its folder item along (untagged now, the
        // child is its member again); a folder item already moving is left to its move.
        n_sounds = static_cast<int>(earlier.size());  // 13-68 v3: the sounds (folder items not counted)
        if (!earlier.empty()) {
            const std::set<MediaItem*> kids(earlier.begin(), earlier.end());
            std::vector<MediaItem*>    folders;
            for (const auto& [key, it] : by_guid) {
                MediaTrack* t = GetMediaItem_Track(it);
                if (moving_guids.count(key) || !t || GetMediaTrackInfo_Value(t, "I_FOLDERDEPTH") != 1.0 || !IsFolderItem(it)) continue;
                for (MediaItem* k : FolderPileMembers(it))
                    if (kids.count(k)) {
                        folders.push_back(it);
                        break;
                    }
            }
            earlier.insert(earlier.end(), folders.begin(), folders.end());
        }
        ToSourceZone(proj, earlier, {});
    }
    return n_sounds;
}

// Populate on `targets`, only the rules holding the library (Antho 2026-10-18: never the project's other
// libraries), then the report as a toast.
// 13-68 v3: both actions count the animations the same way: the items (linked copies included) holding the library.
// 13-68e: `only` (non-null) = only these rules (one side's). v5: `head` (non-empty) = the report's first words instead of
// SpreadRunReport's, Populate's counts after them.
std::string PopulateLibrary(const std::vector<MediaItem*>& targets, const std::string& lib_uid, SpreadRunCounts& counts,
                            const std::set<std::string>* only = nullptr, const std::string& head = std::string())
{
    std::vector<std::string> type_rids;
    counts.animations = 0;
    for (MediaItem* m : WithPoolMembers(targets)) {
        bool holds = false;
        for (const RuleSounds& r : ReadItemSounds(m).rules)
            if (r.library == lib_uid && !r.none && !r.rid.empty() && (!only || only->count(r.rid))) {
                holds = true;
                if (std::find(type_rids.begin(), type_rids.end(), r.rid) == type_rids.end()) type_rids.push_back(r.rid);
            }
        counts.animations += holds ? 1 : 0;
    }
    if (type_rids.empty())
        return Finish((head.empty() ? std::string() : head + ". ") + "No " + counts.type + " rule holds the library " + counts.type +
                      ": nothing placed");
    PopulateCounts    pc;
    int               events = 0;
    const SoundReport rep = PopulateItemsCounted(targets, &events, &pc, false, nullptr, type_rids);
    counts.events = events;
    counts.placed = pc.placed;
    counts.replaced += pc.replaced;
    counts.kept = pc.kept;
    std::string text = SpreadRunReport(counts);
    if (!head.empty()) {
        text = head;
        if (counts.placed > 0) text += ", " + std::to_string(counts.placed) + (counts.placed == 1 ? " sound placed" : " sounds placed");
        if (counts.to_source > 0) text += ", " + std::to_string(counts.to_source) + " earlier to the source zone";
        text += ".";
    }
    for (const ReportLine& l : rep.lines)
        if (l.kind == ReportKind::Blocked) {
            text += "\n" + l.text;
            break;
        }
    UpdateArrange();
    return Finish(text);
}

// 13-68 v3: the library of the tag's rule on its owner ("" when none). 13-68e v5: only for an item its owner's record
// lists (`guid`): a duplicate or a pasted copy carries the original's tag but is a plain new item (am::TagListed).
std::string TagLibrary(ReaProject* proj, const PlacedTag& t, const std::string& guid)
{
    if (t.own == Ownership::Deleted) return std::string();
    MediaItem* o = ScanItemByGuid(proj, t.owner);
    if (!o) return std::string();
    const ItemSounds rec = ReadItemSounds(o);
    if (!am::TagListed(rec, guid)) return std::string();
    for (const RuleSounds& r : rec.rules)
        if (r.rid == t.rid && !r.none) return r.library;
    return std::string();
}


// ---- 13-68e: the Attach manager ----------------------------------------------------------------------------------

// Every project item by GUID (upper case).
std::map<std::string, MediaItem*> ItemsByGuid(ReaProject* proj)
{
    std::map<std::string, MediaItem*> out;
    for (int i = 0, n = CountMediaItems(proj); i < n; ++i)
        if (MediaItem* it = GetMediaItem(proj, i)) out[GuidKey(ItemGuidOf(it))] = it;
    return out;
}

// A library's item GUID (upper case) -> the Sound whose source it is (a folder pile's members too: its Sound's).
std::map<std::string, std::string> SourceNodes(const SoundLibrary& lib, const std::map<std::string, MediaItem*>& items)
{
    std::map<std::string, std::string> out;
    for (const SoundNode& n : lib.nodes) {
        if (n.type != SoundNodeType::Sound || n.sound.source.kind != SoundSourceKind::Item || n.sound.source.item_guid.empty()) continue;
        const std::string key = GuidKey(n.sound.source.item_guid);
        out[key] = n.uid;
        const auto hit = items.find(key);
        if (hit != items.end() && IsFolderItem(hit->second))
            for (MediaItem* k : FolderPileMembers(hit->second)) out[GuidKey(ItemGuidOf(k))] = n.uid;
    }
    return out;
}

// Every item GUID (upper case) some library Sound plays from (its source, a folder pile's members).
std::set<std::string> AllSourceGuids(const SoundStore& st, const std::map<std::string, MediaItem*>& items)
{
    std::set<std::string> out;
    for (const SoundLibrary& l : st.libraries)
        for (const auto& kv : SourceNodes(l, items)) out.insert(kv.first);
    return out;
}

// 13-68e v5: what one read knows of each Attached library (its layout, its sources, an earlier version's layout) and of
// each owner (its item-level values, its rules' sides).
struct LayerCache {
    std::map<std::string, am::Layout>                         lay;
    std::map<std::string, std::map<std::string, std::string>> src;
    std::map<std::string, bool>                               legacy;
    std::map<std::string, std::vector<ValueSet>>              values;  // owner GUID (upper) -> its item values
    std::map<std::string, std::map<std::string, char>>       sides;   // owner GUID (upper) -> rid -> its rule's side
};
// The layer a placed item plays: its tag's node; an original (its tag has no node: it is a Sound's source) = the Sound
// with its source in the content its event plays (its animation's case, its rule's side once separated), never another
// animation's or side's copy of it. None for a library an earlier version laid out (its gestures are refused).
am::LayerAt InstanceLayer(const SoundStore& st, const SoundLibrary& lib, const PlacedTag& t, const std::string& guid_key,
                          const std::map<std::string, MediaItem*>& items, LayerCache& c)
{
    auto lg = c.legacy.find(lib.uid);
    if (lg == c.legacy.end()) lg = c.legacy.emplace(lib.uid, am::LegacyLayout(st, lib, nullptr)).first;
    if (lg->second) return am::LayerAt();
    auto lay = c.lay.find(lib.uid);
    if (lay == c.lay.end()) lay = c.lay.emplace(lib.uid, am::ReadLayout(st, lib)).first;
    std::string node = t.node;
    if (node.empty()) {
        auto s = c.src.find(lib.uid);
        if (s == c.src.end()) s = c.src.emplace(lib.uid, SourceNodes(lib, items)).first;
        const auto hit = s->second.find(guid_key);
        const SoundNode* any = hit == s->second.end() ? nullptr : FindNode(lib, hit->second);
        if (any) {
            const std::string owner = GuidKey(t.owner);
            if (!c.values.count(owner)) {
                const auto o = items.find(owner);
                std::map<std::string, char>& sd = c.sides[owner];
                if (o != items.end()) {
                    c.values[owner] = ReadItemSounds(o->second).item_values;
                    ItemRulesRead rr;
                    if (ReadItemRules(o->second, &rr) && rr.valid)
                        for (const Block& b : rr.rules.blocks)
                            if (!b.rid.empty()) sd[b.rid] = EventSide(b);
                } else {
                    c.values[owner] = {};
                }
            }
            const auto side = c.sides[owner].find(t.rid);
            node = am::PlayedSoundOf(st, lib, lay->second, c.values[owner], side == c.sides[owner].end() ? 0 : side->second,
                                     any->sound.source.item_guid);
            if (node.empty()) node = any->uid;
        }
    }
    return node.empty() ? am::LayerAt() : am::LayerOfNode(lib, lay->second, node);
}

// The project time of clip time `t` on `anim` (its first pass).
double EventPos(MediaItem* anim, double t)
{
    const ItemClipMap m = ItemClipMapOf(anim, 0.0);
    return m.item_pos + (t - m.start_offs) / (m.rate > 0.0 ? m.rate : 1.0);
}

// A marker's name without its preview suffix.
std::string LabelOf(const std::string& name)
{
    std::string n = MarkerTypeName(name);
    const std::string suf = kPreviewSuffix;
    if (n.size() > suf.size() && n.compare(n.size() - suf.size(), suf.size(), suf) == 0) n.resize(n.size() - suf.size());
    return n;
}

// The GUIDs of the RAV project markers the last read listed (polled for their selected label).
std::vector<std::string> g_marker_guids;

// 13-68e v5: "(this instance)" / "(this animation)" / "(the project)".
std::string ScopeText(am::Scope scope)
{
    return std::string(" (") + am::ScopeWords(scope) + ")";
}

am::Scope ScopeOfAction(SpreadScope s)
{
    return s == SpreadScope::AllInstances ? am::Scope::Project : am::Scope::Anim;
}

// Antho 2026-10-19 (Apply to = Anim): "this animation" = every RAV item playing the same animation, by the name RAV
// shows (its take's name, "Two Cycle Sprint"), linked copies included, pooled or not.
std::vector<MediaItem*> SameAnimation(MediaItem* anim)
{
    std::vector<MediaItem*> out = {anim};
    const std::string       name = ItemSoundName(anim);
    for (MediaItem* it : ProjectRavItems())
        if (it != anim && ItemSoundName(it) == name) out.push_back(it);
    return WithPoolMembers(out);
}

// The animation an Anim gesture acts for: its name, the item-level values its items hold, the values held by RAV items
// of another animation (never reused), the kind's name (the AnimSwitch's group "<kind> animations").
am::PoolKey PoolKeyOf(MediaItem* anim, const std::string& kind_name)
{
    am::PoolKey p;
    p.anim_name = ItemSoundName(anim);
    p.kind_name = kind_name;
    const std::vector<MediaItem*> same = SameAnimation(anim);
    const std::set<MediaItem*>    mine(same.begin(), same.end());
    for (MediaItem* it : same)
        for (const ValueSet& v : ReadItemSounds(it).item_values) p.item_values.push_back(v);
    for (MediaItem* it : ProjectRavItems())
        if (!mine.count(it))
            for (const ValueSet& v : ReadItemSounds(it).item_values) p.held_elsewhere.insert(v.value_uid);
    return p;
}

// Every item's tag of `guids` renamed by `uids` (RemapTagNodes).
void RemapTags(ReaProject* proj, const std::set<std::string>& guids, const std::map<std::string, std::string>& uids)
{
    if (guids.empty() || uids.empty()) return;
    for (int i = 0, n = CountMediaItems(proj); i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        PlacedTag  t;
        if (!it || !guids.count(GuidKey(ItemGuidOf(it))) || !ReadPlacedTag(it, &t)) continue;
        if (am::RemapTagNodes(t, uids)) WritePlacedTag(it, t);
    }
}

// After an edit (EditOut): an Anim gesture's value on the animation item and its linked copies, their records and tags
// renamed to their own copy (remap_pool), and Separate's R copy on every R event holding the library (remap_side).
void ApplyEdit(ReaProject* proj, MediaItem* anim, const std::string& lib_uid, const std::string& kind_key, const am::EditOut& o)
{
    std::set<std::string> guids;
    if (anim && !o.anim_value.group_uid.empty())
        for (MediaItem* m : SameAnimation(anim))  // every item of that animation shares its value
            ModifyItemSounds(m, nullptr, [&](ItemSounds& r) {
                bool changed = SetValueIn(r.item_values, o.anim_value.group_uid, o.anim_value.value_uid);
                if (!o.remap_pool.empty())
                    for (EventSounds& ev : r.events)
                        if (am::RemapEventNodes(ev, o.remap_pool)) {
                            changed = true;
                            for (const SoundPlacement& p : ev.placed) guids.insert(GuidKey(p.item_guid));
                        }
                return changed;
            });
    RemapTags(proj, guids, o.remap_pool);
    if (o.remap_side.empty()) return;
    guids.clear();
    for (MediaItem* it : LibraryTargets({}, lib_uid)) {
        const std::set<std::string> r_rids = SideRids({it}, kind_key, 'R');
        ModifyItemSounds(it, nullptr, [&](ItemSounds& r) {
            bool changed = false;
            for (EventSounds& ev : r.events)
                if (r_rids.count(ev.rid) && am::RemapEventNodes(ev, o.remap_side)) {
                    changed = true;
                    for (const SoundPlacement& p : ev.placed) guids.insert(GuidKey(p.item_guid));
                }
            return changed;
        });
    }
    RemapTags(proj, guids, o.remap_side);
}

// The rules a gesture empties and places again: every rule of the kind when it wrote both sides (slot 0), else the
// sides written. Null = every rule of the library.
const std::set<std::string>* OnlyRids(const am::EditOut& o, const std::vector<MediaItem*>& targets, const std::string& kind_key,
                                      std::set<std::string>* store)
{
    if (o.slots.empty() || o.slots.count(0)) return nullptr;
    for (char s : o.slots) {
        const std::set<std::string> r = SideRids(targets, kind_key, s);
        store->insert(r.begin(), r.end());
    }
    return store;
}

// Item scope: the sounds the marker had leave it (an attached / spread sound to the source zone, untagged; a populated
// copy deleted), so the selection is that marker's sound alone. Returns how many.
int ClearEvent(ReaProject* proj, MediaItem* owner, const std::string& rid, double t, const std::vector<MediaItem*>& keep)
{
    const ItemSounds   rec = ReadItemSounds(owner);
    const EventSounds* e = FindEventSounds(rec, rid, t);
    if (!e) return 0;
    std::set<std::string> keep_guids;
    for (MediaItem* it : keep) keep_guids.insert(GuidKey(ItemGuidOf(it)));
    const std::map<std::string, MediaItem*> items = ItemsByGuid(proj);
    std::vector<MediaItem*>                 to_zone;
    std::vector<std::string>                gone;
    for (const SoundPlacement& p : e->placed) {
        if (p.own == Ownership::Deleted) continue;
        const std::string key = GuidKey(p.item_guid);
        const auto        hit = items.find(key);
        if (keep_guids.count(key)) continue;
        gone.push_back(p.item_guid);
        if (hit == items.end()) continue;  // its item no longer exists: only its placement goes
        if (p.own == Ownership::Populated && !IsFolderItem(hit->second) && RemovePlacedAny(proj, hit->second)) continue;
        ClearPlacedTag(hit->second);
        to_zone.push_back(hit->second);
    }
    if (gone.empty()) return 0;
    ModifyItemSounds(owner, nullptr, [&](ItemSounds& r) {
        bool any = false;
        for (const std::string& g : gone) any = DetachFromRecord(r, g) || any;
        if (EventSounds* ev = FindEventSounds(r, rid, t)) ev->draws.clear();
        return any;
    });
    ToSourceZone(proj, to_zone, {});
    return static_cast<int>(gone.size());
}

// Attach (13-68 v3, 13-68e v5): the selection onto the marker (the window's, else the clicked label) and, in Anim /
// Project, onto every marker of its kind in scope: L and R together until separated, else the marker's side. The
// first pile moves onto the marker, attached there; the other piles go to the source zone. Item: that marker only,
// the library unchanged. One undo point.
std::string RunAttach(am::Scope scope, const SpreadMarker* given)
{
    if (!f_set_item) return Finish("REAPER functions missing");
    // P3b: no project marker to select (13-68e: the window passes its marker).
    if (!given && GetTaggingMarkerMode() == MarkerMode::Take) return Finish(kTakeOnly);
    ReaProject*                   proj = EnumProjects(-1, nullptr, 0);
    const std::vector<MediaItem*> rav_items = ProjectRavItems();
    SelectedMarker                mk;
    int                           n_selected = 0;
    if (given) {
        if (!given->item || !IsLiveItem(given->item)) return Finish("The marker's animation item is gone: nothing changed");
        mk.item = given->item;
        mk.name = given->name;
        mk.pos = given->pos;
        n_selected = 1;
    } else {
        n_selected = FindSelectedMarker(rav_items, &mk);
    }
    if (n_selected == 0 || !mk.item) return Finish(kNoMarker);
    // 13-68 v3: its kind with its sides (from its rule): an L marker's kind is also its R rules'.
    SpreadKind kind;
    int        mk_block = -1;
    char       side = 0;
    {
        ItemRulesRead rr;
        if (ReadItemRules(mk.item, &rr) && rr.valid) {
            kind = KindOfRuleMarker(rr.rules, mk.name, &mk_block);
            if (mk_block >= 0) side = EventSide(rr.rules.blocks[static_cast<size_t>(mk_block)]);
        }
    }
    if (kind.key.empty()) return Finish("No rule of this animation writes the marker " + MarkerTypeName(mk.name));
    const std::string& type = kind.name;
    std::vector<SelectedSound> sel;
    int                        skipped = 0;
    std::string                skip;
    ReadSelectedSounds(&sel, &skipped, &skip, true, true);
    if (sel.empty()) return Finish("Select the sound items first" + (skip.empty() ? std::string() : " (" + skip + ")"));
    std::vector<AttachItem> facts;
    for (const SelectedSound& s : sel) facts.push_back(s.facts);

    // The animation items: this one (Populate adds its linked copies), or every RAV item with a rule of the kind.
    // Anim: every item of the marker's animation (same name, linked copies included); Project: every one with the kind.
    std::vector<MediaItem*> anims = scope == am::Scope::Anim ? SameAnimation(mk.item) : std::vector<MediaItem*>{mk.item};
    if (scope == am::Scope::Project)
        for (MediaItem* it : rav_items) {
            if (it == mk.item) continue;
            ItemRulesRead rr;
            if (!ReadItemRules(it, &rr) || !rr.valid) continue;
            const std::vector<char> on = BlocksOfType(rr.rules, kind.key);
            if (std::find(on.begin(), on.end(), 1) != on.end()) anims.push_back(it);
        }

    std::vector<AttachEvent> type_events;
    {
        BusyScope busy("Reading markers");
        MarkersOfType(mk.item, kind.key, &type_events);
    }
    if (type_events.empty()) {
        ItemRulesRead rr;
        if (ReadItemRules(mk.item, &rr) && rr.valid)
            for (const Block& b : rr.rules.blocks)
                if (b.enabled && KindOfBlock(b).key == kind.key && IsAutoBlock(b) && !IsActiveAutoBlock(b))
                    return Finish("The " + type + " rule is from an older RAV: press Detect on this animation");
        return Finish("No " + type + " marker on this animation");
    }
    // The event nearest the marker, of its own rule first (the other side's may sit close).
    AttachEvent at_marker = type_events.front();
    bool        own_rule = at_marker.block == mk_block;
    for (const AttachEvent& e : type_events) {
        const bool mine = e.block == mk_block;
        if ((mine && !own_rule) || (mine == own_rule && std::fabs(e.project_t - mk.pos) < std::fabs(at_marker.project_t - mk.pos))) {
            at_marker = e;
            own_rule = mine;
        }
    }
    const std::vector<std::vector<int>> piles = SpreadPiles(facts);
    const std::vector<int>              first_pile = piles.empty() ? std::vector<int>() : piles.front();
    const int                           first_top = TopmostOf(facts, first_pile);
    const double first_anchor = first_top >= 0 ? PileAnchor(facts[static_cast<size_t>(first_top)]) : at_marker.project_t;
    const ReadSounds rs = ReadSources(sel, first_pile);
    const am::PoolKey pool = scope == am::Scope::Item ? am::PoolKey() : PoolKeyOf(mk.item, type);

    const std::string undo = "RAV: Attach to " + MarkerTypeName(mk.name) + ScopeText(scope);
    SoundUndoScope    scope_undo(proj, undo.c_str());
    std::vector<std::vector<std::string>> rids_of(anims.size());
    for (size_t i = 0; i < anims.size(); ++i) rids_of[i] = TypeRids(anims[i], kind.key);
    if (rids_of[0].empty()) return Finish("Could not give the " + type + " rules an id: nothing changed");
    at_marker.rid = EventRid(mk.item, at_marker);
    if (at_marker.rid.empty()) return Finish("Could not give the " + MarkerTypeName(mk.name) + " rule an id: nothing changed");

    SpreadRunCounts counts;
    counts.type = type;
    counts.marker = MarkerTypeName(mk.name);
    counts.all_instances = scope == am::Scope::Project;
    counts.item = scope == am::Scope::Item;
    counts.selected_markers = n_selected;
    counts.variations = static_cast<int>(piles.size());
    counts.layers = static_cast<int>(sel.size());
    counts.skip_note = skip;

    if (scope == am::Scope::Item) {  // this marker only: the library and the other markers unchanged
        const SoundLibrary* lib = FindAttachedLibrary(GetSoundStore(proj), kind.key);
        LeaveOldPlaces(proj, sel);
        const int cleared = ClearEvent(proj, mk.item, at_marker.rid, at_marker.clip_t, rs.attach_items);
        const double delta = at_marker.project_t - first_anchor;
        for (MediaItem* it : rs.first_items) f_set_item(it, "D_POSITION", GetMediaItemInfo_Value(it, "D_POSITION") + delta);
        ToSourceZone(proj, rs.to_move, rs.first_items);
        AttachToEvent(proj, mk.item, at_marker, lib ? lib->uid : std::string(), rs.attach_items, 0.0, rs.first_folders);
        counts.animations = 1;
        counts.events = 1;
        counts.reused = lib != nullptr;
        counts.to_source = cleared;
        UpdateArrange();
        return Finish(SpreadRunReport(counts));
    }

    // The kind's Attached library (one for both sides, side-free name): made, or its content in scope replaced.
    std::string lib_uid;
    am::EditOut edit;
    {
        const std::vector<SoundNode> layer = SpreadLayerNodes(rs.sources, type, &NewSoundUid);
        const bool                   wrote = ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
            SoundLibrary* lib = FindAttachedLibrary(st, kind.key);
            counts.reused = lib != nullptr;
            if (!lib) {
                SoundLibrary l;
                l.uid = NewSoundUid();
                l.name = type;
                l.attached = true;
                st.libraries.push_back(l);
                lib = &st.libraries.back();
            }
            lib->attached_type = kind.key;  // found by it from now on, whatever its name
            lib_uid = lib->uid;
            edit = am::AttachContent(st, lib_uid, scope, pool, side, layer, &NewSoundUid);
            return edit.ok;
        });
        if (!wrote || lib_uid.empty())
            return Finish("Could not write the library " + type + ": " + (edit.why.empty() ? std::string("nothing changed") : edit.why));
    }
    // Which side was written (a separated Group: the marker's side only).
    if (!edit.slots.empty() && !edit.slots.count(0)) counts.side = side;

    // The assignment: every rule of the kind (both sides), on each animation item and its linked copies.
    for (size_t i = 0; i < anims.size(); ++i) {
        const std::vector<std::string>& of_type = rids_of[i];
        if (of_type.empty()) continue;
        for (MediaItem* m : WithPoolMembers({anims[i]}))
            ModifyItemSounds(m, nullptr, [&](ItemSounds& r) { return SetRulesLibrary(r, of_type, LibState::Library, lib_uid); });
    }
    ApplyEdit(proj, scope == am::Scope::Anim ? mk.item : nullptr, lib_uid, kind.key, edit);
    ApplySideValues(proj, lib_uid);  // each rule plays its side's content
    LeaveOldPlaces(proj, sel);
    {
        const double delta = at_marker.project_t - first_anchor;
        for (MediaItem* it : rs.first_items) f_set_item(it, "D_POSITION", GetMediaItemInfo_Value(it, "D_POSITION") + delta);
    }
    ToSourceZone(proj, rs.to_move, rs.first_items);

    // Anim: this animation (Populate adds its linked copies); Project: every item holding the library.
    const std::vector<MediaItem*> targets = scope == am::Scope::Anim ? SameAnimation(mk.item) : LibraryTargets(anims, lib_uid);
    std::set<std::string>         only_store;
    const std::set<std::string>*  only = OnlyRids(edit, targets, kind.key, &only_store);
    counts.to_source = DropEarlierSounds(proj, targets, lib_uid, only, rs.to_move);
    AttachToEvent(proj, mk.item, at_marker, lib_uid, rs.attach_items, 0.0, rs.first_folders);
    return PopulateLibrary(targets, lib_uid, counts, only);
}

// Add to pile (13-68 v3 "Add to stack", 13-68e v5): the selection holds the new items and one or more instances of
// one marker (an Attach's original or a placed copy; several markers: refused). The new items become a layer at their
// offset from that instance's event (top item's snap minus the event): Anim = in the animation's Group, Project = in
// every Group; the pile nearest the event stays there, attached; the other piles go to the source zone; Populate
// (that library's rules only). Item: the new items on that marker only, the library unchanged.
std::string RunPile(am::Scope scope)
{
    if (!f_set_item) return Finish("REAPER functions missing");
    ReaProject*      proj = EnumProjects(-1, nullptr, 0);
    const SoundStore store = GetSoundStore(proj);
    // The selected instances: one layer's linked instances (selected together by hand) or those of one marker.
    std::vector<PlacedTag>                  tags;
    std::vector<am::StackRef>               refs;
    std::string                             lib_uid, key;
    const std::map<std::string, MediaItem*> items = ItemsByGuid(proj);
    LayerCache                              layers;
    for (int i = 0, n = CountSelectedMediaItems(proj); i < n; ++i) {
        MediaItem* it = GetSelectedMediaItem(proj, i);
        PlacedTag  t;
        if (!it || !ReadPlacedTag(it, &t)) continue;
        const SoundLibrary* l = FindLibrary(store, TagLibrary(proj, t, ItemGuidOf(it)));
        if (!l || !l->attached || l->attached_type.empty()) continue;
        am::StackRef r;
        r.owner = t.owner;
        r.rid = t.rid;
        r.t = t.t;
        const am::LayerAt la = InstanceLayer(store, *l, t, GuidKey(ItemGuidOf(it)), items, layers);
        if (!la.layer.empty()) r.layer = l->uid + "|" + la.layer;
        if (MediaItem* o = ScanItemByGuid(proj, t.owner)) r.pos = EventPos(o, t.t);
        if (lib_uid.empty()) {
            lib_uid = l->uid;
            key = l->attached_type;
        }
        tags.push_back(t);
        refs.push_back(r);
    }
    if (refs.empty()) return Finish("Select one attached instance (a sound the Attach placed) with the items to add");
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    MediaItem* const    first_owner = ScanItemByGuid(proj, tags.front().owner);
    const std::string   type = first_owner ? KindNameOn(first_owner, key, lib ? lib->name : key) : (lib ? lib->name : key);
    std::vector<SelectedSound> all_sel, sel;
    int                        skipped = 0;
    std::string                skip;
    ReadSelectedSounds(&all_sel, &skipped, &skip, true, true);
    for (const SelectedSound& s : all_sel) {
        const bool placed_for = s.tagged && TagLibrary(proj, s.tag, s.facts.guid) == lib_uid;
        bool       kid_in = false;
        if (s.folder)
            for (MediaItem* k : FolderPileMembers(s.item)) {
                PlacedTag kt;
                if (ReadPlacedTag(k, &kt) && TagLibrary(proj, kt, ItemGuidOf(k)) == lib_uid) kid_in = true;
            }
        if (!lib || !IsStackItem(*lib, s.facts.guid, placed_for, kid_in)) sel.push_back(s);
    }
    if (sel.empty()) return Finish("Select the items to add with one attached instance");
    // The reference: the instance whose marker is nearest the new items (their first pile's top snap).
    double anchor = 0.0;
    bool   has_anchor = false;
    {
        std::vector<AttachItem> f;
        for (const SelectedSound& s : sel) f.push_back(s.facts);
        const std::vector<std::vector<int>> pl = SpreadPiles(f);
        const int top = pl.empty() ? -1 : TopmostOf(f, pl.front());
        if (top >= 0) {
            anchor = PileAnchor(f[static_cast<size_t>(top)]);
            has_anchor = true;
        }
    }
    int       distinct = 0;
    const int at_ref = am::PileReference(refs, has_anchor, anchor, &distinct);
    if (at_ref < 0)
        return Finish("The selected instances are of " + std::to_string(distinct) +
                      " different layers or markers: select instances of one layer. Nothing changed");
    const PlacedTag ref = tags[static_cast<size_t>(at_ref)];
    MediaItem*      owner = ScanItemByGuid(proj, ref.owner);
    if (!owner) return Finish("The instance's animation item is gone: nothing changed");
    char side = 0;  // the instance's side, from its rule
    {
        ItemRulesRead rr;
        if (ReadItemRules(owner, &rr) && rr.valid)
            for (const Block& b : rr.rules.blocks)
                if (!b.rid.empty() && b.rid == ref.rid) side = EventSide(b);
    }
    const am::PoolKey pool = scope == am::Scope::Item ? am::PoolKey() : PoolKeyOf(owner, type);
    const std::string undo = "RAV: Add to pile " + type + ScopeText(scope);
    SoundUndoScope    scope_undo(proj, undo.c_str());
    AttachEvent ev;
    bool        found = false;
    {
        BusyScope                busy("Reading markers");
        std::vector<std::string> rids;
        std::string              err;
        std::vector<AttachEvent> evs;
        if (ItemRuleIds(owner, &rids, &err) && MarkersOfType(owner, key, &evs))
            for (const AttachEvent& e : evs)
                if (e.block >= 0 && static_cast<size_t>(e.block) < rids.size() && rids[static_cast<size_t>(e.block)] == ref.rid &&
                    std::fabs(e.clip_t - ref.t) <= kEventMatchSeconds) {
                    ev = e;
                    ev.rid = ref.rid;
                    found = true;
                }
    }
    if (!found) return Finish("The instance's " + type + " marker is not on its animation now: nothing changed");

    std::vector<AttachItem> facts;
    for (const SelectedSound& s : sel) facts.push_back(s.facts);
    const std::vector<std::vector<int>> piles = SpreadPiles(facts);
    const int                           at = NearestPile(facts, piles, ev.project_t);
    const std::vector<int>              first_pile = at >= 0 ? piles[static_cast<size_t>(at)] : std::vector<int>();
    const int                           first_top = TopmostOf(facts, first_pile);
    if (first_top < 0) return Finish("Select the items to add with one attached instance");
    const double     offset_s = PileAnchor(facts[static_cast<size_t>(first_top)]) - ev.project_t;
    const ReadSounds rs = ReadSources(sel, first_pile);

    SpreadRunCounts counts;
    counts.type = type;
    counts.add_layer = true;
    counts.all_instances = scope == am::Scope::Project;
    counts.item = scope == am::Scope::Item;
    counts.reused = true;
    counts.variations = static_cast<int>(piles.size());
    counts.layers = static_cast<int>(sel.size());
    counts.skip_note = skip;
    if (scope == am::Scope::Item) {  // this instance only: the new items on its marker, the library unchanged
        LeaveOldPlaces(proj, sel);
        ToSourceZone(proj, rs.to_move, rs.first_items);
        AttachToEvent(proj, owner, ev, lib_uid, rs.attach_items, offset_s, rs.first_folders);
        counts.animations = 1;
        counts.events = 1;
        UpdateArrange();
        return Finish(SpreadRunReport(counts));
    }
    am::EditOut edit;
    {
        std::vector<SoundNode> layer = SpreadLayerNodes(rs.sources, type, &NewSoundUid);
        const bool             wrote = ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
            edit = am::AddPileLayer(st, lib_uid, scope, pool, side, layer, offset_s * 1000.0, &NewSoundUid);
            return edit.ok;
        });
        if (!wrote) return Finish("Could not write the library " + type + ": " + (edit.why.empty() ? std::string("nothing changed") : edit.why));
    }
    if (!edit.slots.empty() && !edit.slots.count(0)) counts.side = side;
    ApplyEdit(proj, scope == am::Scope::Anim ? owner : nullptr, lib_uid, key, edit);
    ApplySideValues(proj, lib_uid);
    LeaveOldPlaces(proj, sel);
    ToSourceZone(proj, rs.to_move, rs.first_items);
    AttachToEvent(proj, owner, ev, lib_uid, rs.attach_items, offset_s, rs.first_folders);
    const std::vector<MediaItem*> targets = scope == am::Scope::Anim ? SameAnimation(owner) : LibraryTargets({owner}, lib_uid);
    std::set<std::string>         only_store;
    return PopulateLibrary(targets, lib_uid, counts, OnlyRids(edit, targets, key, &only_store));
}

// The marker the L and R card acts from: its animation item, its kind and side, the kind's Attached library.
struct LrTarget {
    MediaItem*  item = nullptr;
    SpreadKind  kind;
    char        side = 0;
    std::string lib_uid;
};
std::string ReadLrTarget(const SpreadMarker& m, LrTarget* out)
{
    if (!m.item || !IsLiveItem(m.item)) return "The marker's animation item is gone: nothing changed";
    out->item = m.item;
    ItemRulesRead rr;
    int           block = -1;
    if (ReadItemRules(m.item, &rr) && rr.valid) {
        out->kind = KindOfRuleMarker(rr.rules, m.name, &block);
        if (block >= 0) out->side = EventSide(rr.rules.blocks[static_cast<size_t>(block)]);
    }
    if (out->kind.key.empty()) return "No rule of this animation writes the marker " + MarkerTypeName(m.name);
    const SoundLibrary* lib = FindAttachedLibrary(GetSoundStore(nullptr), out->kind.key);
    if (!lib) return "Nothing attached on " + out->kind.name + " yet: nothing changed";
    out->lib_uid = lib->uid;
    return std::string();
}

// Separate L and R (Anim / Project): the same sound on both sides, two groups from now on. No sound moves.
std::string RunSeparate(am::Scope scope, const SpreadMarker& m)
{
    LrTarget          t;
    const std::string err = ReadLrTarget(m, &t);
    if (!err.empty()) return Finish(err);
    if (!t.side) return Finish("The " + t.kind.name + " marker has no side: nothing to separate");
    ReaProject*       proj = EnumProjects(-1, nullptr, 0);
    const am::PoolKey pool = PoolKeyOf(t.item, t.kind.name);
    const std::string undo = "RAV: Separate L and R " + t.kind.name + ScopeText(scope);
    SoundUndoScope    scope_undo(proj, undo.c_str());
    am::EditOut       edit;
    const bool        wrote = ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
        edit = am::SeparateSides(st, t.lib_uid, scope, pool, &NewSoundUid);
        return edit.ok;
    });
    if (!wrote) return Finish(undo + ": " + (edit.why.empty() ? std::string("nothing changed") : edit.why) + ".");
    ApplyEdit(proj, scope == am::Scope::Anim ? t.item : nullptr, t.lib_uid, t.kind.key, edit);
    ApplySideValues(proj, t.lib_uid);
    UpdateArrange();
    return Finish(undo + ": same sound for now, L and R are two groups.");
}

// Merge L and R (Anim / Project): both sides play the chosen marker's side's sound; the other side's are replaced.
std::string RunMerge(am::Scope scope, const SpreadMarker& m)
{
    LrTarget          t;
    const std::string err = ReadLrTarget(m, &t);
    if (!err.empty()) return Finish(err);
    if (!t.side) return Finish("The " + t.kind.name + " marker has no side: nothing to merge");
    ReaProject*       proj = EnumProjects(-1, nullptr, 0);
    const am::PoolKey pool = PoolKeyOf(t.item, t.kind.name);
    const std::string undo = "RAV: Merge L and R " + t.kind.name + ScopeText(scope);
    SoundUndoScope    scope_undo(proj, undo.c_str());
    am::EditOut       edit;
    const bool        wrote = ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
        edit = am::MergeSides(st, t.lib_uid, scope, pool, t.side, &NewSoundUid);
        return edit.ok;
    });
    if (!wrote) return Finish(undo + ": " + (edit.why.empty() ? std::string("nothing changed") : edit.why) + ".");
    ApplyEdit(proj, scope == am::Scope::Anim ? t.item : nullptr, t.lib_uid, t.kind.key, edit);
    const std::vector<MediaItem*> targets = scope == am::Scope::Anim ? SameAnimation(t.item) : LibraryTargets({}, t.lib_uid);
    const std::set<std::string>   other = SideRids(targets, t.kind.key, edit.dropped_side);
    SpreadRunCounts counts;
    counts.type = t.kind.name;
    counts.reused = true;
    counts.all_instances = scope == am::Scope::Project;
    counts.to_source = DropEarlierSounds(proj, targets, t.lib_uid, &other, {});
    const std::string head = undo + ": " + std::string(1, edit.dropped_side) + " markers now play the " + std::string(1, t.side) + " sound";
    return PopulateLibrary(targets, t.lib_uid, counts, &other, head);
}

void ReadManager(AttachManagerState* out)
{
    *out = AttachManagerState();
    am::Snapshot&     s = out->snap;
    ReaProject*       proj = EnumProjects(-1, nullptr, 0);
    const SoundStore& store = GetSoundStore(proj);
    const std::map<std::string, MediaItem*> items = ItemsByGuid(proj);
    out->anims = ProjectRavItems();
    double a = 0.0, b = 0.0;
    GetSet_LoopTimeRange2(proj, false, false, &a, &b, false);
    s.has_time_sel = b > a + 1e-9;
    s.ts0 = a;
    s.ts1 = b;
    for (int i = 0, n = CountSelectedMediaItems(proj); i < n; ++i)
        if (MediaItem* it = GetSelectedMediaItem(proj, i)) s.selected_items.insert(GuidKey(ItemGuidOf(it)));
    g_marker_guids.clear();
    std::map<std::string, std::map<std::string, std::string>> node_of;  // library uid -> its source nodes
    auto nodes_of = [&](const SoundLibrary& l) -> const std::map<std::string, std::string>& {
        auto hit = node_of.find(l.uid);
        if (hit == node_of.end()) hit = node_of.emplace(l.uid, SourceNodes(l, items)).first;
        return hit->second;
    };
    for (size_t ai = 0; ai < out->anims.size(); ++ai) {
        MediaItem* it = out->anims[ai];
        am::Anim   an;
        an.guid = ItemGuidOf(it);
        an.name = ItemSoundName(it);
        an.selected = GetMediaItemInfo_Value(it, "B_UISEL") != 0.0;
        an.values = ReadItemSounds(it).item_values;  // 13-68e v5: its per-animation value (its Group)
        s.anims.push_back(an);
        ItemRulesRead rr;
        if (!ReadItemRules(it, &rr) || !rr.valid) continue;
        std::vector<std::string> names;
        for (const Block& bl : rr.rules.blocks) names.push_back(bl.marker);
        // Its RAV project markers (committed and previews): in the time selection, or clicked.
        bool any_ref = false;
        for (const std::vector<ProjectMarkerRef>* refs : {&rr.rules.pmarkers, &rr.rules.ppmarkers})
            for (const ProjectMarkerRef& r : *refs) {
                if (r.guid.empty()) continue;
                any_ref = true;
                g_marker_guids.push_back(r.guid);
                double     pos = r.t;
                const bool clicked = ProjectMarkerSelected(r.guid, &pos);
                const bool in = s.has_time_sel && pos >= a - 1e-9 && pos <= b + 1e-9;
                if (!clicked && !in) continue;
                const int blk = RuleOfMarker(names, r.name);
                if (blk < 0 || !rr.rules.blocks[static_cast<size_t>(blk)].enabled) continue;
                const Block&     bl = rr.rules.blocks[static_cast<size_t>(blk)];
                const SpreadKind k = KindOfBlock(bl);
                if (k.key.empty()) continue;
                am::Marker m;
                m.anim = static_cast<int>(ai);
                m.name = r.name;
                m.label = LabelOf(r.name);
                m.kind_key = k.key;
                m.kind_name = k.name;
                m.side = EventSide(bl);
                m.pos = pos;
                m.clicked = clicked;
                s.markers.push_back(m);
            }
        // Markers = Take only (no project marker): the item's events, when it is in the time selection.
        if (!any_ref && s.has_time_sel) {
            const double p0 = GetMediaItemInfo_Value(it, "D_POSITION"), p1 = p0 + GetMediaItemInfo_Value(it, "D_LENGTH");
            if (p1 > a && p0 < b) {
                std::vector<AttachEvent> evs;
                const ItemDetection      det = DetectItem(it);
                if (det.status == ItemDetection::Status::Ok) {
                    const std::vector<ShownEvent> shown = BuildEventList(det.events, det.rules.events, det.rules.blocks.size());
                    for (const AttachEvent& e : AttachEventsOf(shown, det.map)) {
                        if (e.block < 0 || static_cast<size_t>(e.block) >= det.rules.blocks.size()) continue;
                        if (e.project_t < a - 1e-9 || e.project_t > b + 1e-9) continue;
                        const Block& bl = det.rules.blocks[static_cast<size_t>(e.block)];
                        const SpreadKind k = KindOfBlock(bl);
                        if (!bl.enabled || k.key.empty()) continue;
                        am::Marker m;
                        m.anim = static_cast<int>(ai);
                        m.name = bl.marker;
                        m.label = MarkerTypeName(bl.marker);
                        m.kind_key = k.key;
                        m.kind_name = k.name;
                        m.side = EventSide(bl);
                        m.pos = e.project_t;
                        s.markers.push_back(m);
                    }
                }
            }
        }
        // Its events holding sounds of an Attached library.
        const ItemSounds rec = ReadItemSounds(it);
        for (const EventSounds& ev : rec.events) {
            const RuleSounds* rs = FindRuleSounds(rec, ev.rid);
            if (!rs || rs->none) continue;
            const SoundLibrary* lib = FindLibrary(store, rs->library);
            if (!lib || !lib->attached) continue;
            const std::map<std::string, std::string>& src = nodes_of(*lib);
            am::AttachedEvent e;
            e.anim = static_cast<int>(ai);
            e.rid = ev.rid;
            e.t = ev.t;
            e.pos = EventPos(it, ev.t);
            e.lib_uid = lib->uid;
            e.draws = ev.draws;
            for (const Block& bl : rr.rules.blocks)
                if (bl.rid == ev.rid) {
                    e.label = MarkerTypeName(bl.marker);
                    e.kind_name = KindOfBlock(bl).name;
                    e.side = EventSide(bl);
                }
            for (const SoundPlacement& p : ev.placed) {
                if (p.own == Ownership::Deleted) continue;
                const auto hit = items.find(GuidKey(p.item_guid));
                if (hit == items.end()) continue;  // gone: the follow marks it deleted
                am::Placed x;
                x.item_guid = p.item_guid;
                x.name = ItemSoundName(hit->second);
                x.node = p.node;
                if (x.node.empty()) {
                    const auto n = src.find(GuidKey(p.item_guid));
                    if (n != src.end()) x.node = n->second;
                }
                x.own = p.own;
                x.offset_s = p.offset_s;
                e.placed.push_back(x);
            }
            if (!e.placed.empty()) s.events.push_back(e);
        }
    }
    if (GetTaggingMarkerMode() == MarkerMode::Take && s.markers.empty() && s.has_time_sel)
        s.take_only = "No RAV marker found in the time selection (Markers = Take only: none of these items detects one here).";

    // The selection: what Attach takes, and the stack items with what Add to stack would add.
    std::vector<SelectedSound> sel;
    int                        skipped = 0;
    std::string                skip;
    ReadSelectedSounds(&sel, &skipped, &skip, true, true);
    s.sounds = static_cast<int>(sel.size());
    std::vector<AttachItem> facts;
    for (const SelectedSound& x : sel) facts.push_back(x.facts);
    s.piles = static_cast<int>(SpreadPiles(facts).size());
    std::string stack_lib;
    LayerCache  layers;
    for (int i = 0, n = CountSelectedMediaItems(proj); i < n; ++i) {
        MediaItem* it = GetSelectedMediaItem(proj, i);
        PlacedTag  t;
        if (!it || !ReadPlacedTag(it, &t)) continue;
        const SoundLibrary* l = FindLibrary(store, TagLibrary(proj, t, ItemGuidOf(it)));
        if (!l || !l->attached || l->attached_type.empty()) continue;
        if (stack_lib.empty()) stack_lib = l->uid;
        am::StackRef r;
        r.owner = t.owner;
        r.rid = t.rid;
        r.t = t.t;
        const am::LayerAt la = InstanceLayer(store, *l, t, GuidKey(ItemGuidOf(it)), items, layers);  // v5: linked instances
        if (!la.layer.empty()) r.layer = l->uid + "|" + la.layer;
        if (MediaItem* o = ScanItemByGuid(proj, t.owner)) r.pos = EventPos(o, t.t);
        for (size_t k = 0; k < s.events.size(); ++k) {
            const am::AttachedEvent& e = s.events[k];
            if (e.rid == r.rid && std::fabs(e.t - r.t) <= kEventMatchSeconds && e.anim >= 0 &&
                GuidKey(s.anims[static_cast<size_t>(e.anim)].guid) == GuidKey(r.owner))
                r.event = static_cast<int>(k);
        }
        s.stack_refs.push_back(r);
    }
    std::vector<AttachItem> fresh_facts;
    if (const SoundLibrary* l = stack_lib.empty() ? nullptr : FindLibrary(store, stack_lib))
        for (const SelectedSound& x : sel) {
            const bool placed_for = x.tagged && TagLibrary(proj, x.tag, x.facts.guid) == stack_lib;
            bool       kid_in = false;
            if (x.folder)
                for (MediaItem* k : FolderPileMembers(x.item)) {
                    PlacedTag kt;
                    if (ReadPlacedTag(k, &kt) && TagLibrary(proj, kt, ItemGuidOf(k)) == stack_lib) kid_in = true;
                }
            if (!IsStackItem(*l, x.facts.guid, placed_for, kid_in)) {
                ++s.fresh;
                fresh_facts.push_back(x.facts);
            }
        }
    // 13-68e v5: the new items' anchor (the first pile's top snap), the reference instance nearest it (PileReference),
    // and Add to pile's offset (the new pile nearest that instance's event: its top snap minus the event).
    if (!fresh_facts.empty()) {
        const std::vector<std::vector<int>> piles = SpreadPiles(fresh_facts);
        const int top = piles.empty() ? -1 : TopmostOf(fresh_facts, piles.front());
        if (top >= 0) {
            s.fresh_anchor = PileAnchor(fresh_facts[static_cast<size_t>(top)]);
            s.has_fresh_anchor = true;
        }
    }
    const int ref = am::PileReference(s.stack_refs, s.has_fresh_anchor, s.fresh_anchor, nullptr);
    if (ref >= 0 && s.stack_refs[static_cast<size_t>(ref)].event >= 0 && !fresh_facts.empty()) {
        const am::AttachedEvent&            e = s.events[static_cast<size_t>(s.stack_refs[static_cast<size_t>(ref)].event)];
        const std::vector<std::vector<int>> piles = SpreadPiles(fresh_facts);
        const int                           at = NearestPile(fresh_facts, piles, e.pos);
        const int top = at >= 0 ? TopmostOf(fresh_facts, piles[static_cast<size_t>(at)]) : -1;
        if (top >= 0) {
            s.pile_offset_s = PileAnchor(fresh_facts[static_cast<size_t>(top)]) - e.pos;
            s.has_pile_offset = true;
        }
    }
}



// ---- 13-68e v5: linked instances, gestures made in REAPER (a timer: also with the window closed) ---------------
// The decisions are attach_manager_plan.h DecideWatch's (pure, host-tested); here the reads and the writes.

// What the watcher last read of the current project, and the originals (items a library plays from) as they were,
// so one deleted by hand can be put back in the source zone.
struct WatchRead {
    am::WatchSnap                      snap;
    std::map<std::string, std::string> chunks;  // original's GUID (upper case) -> its state chunk
    std::map<std::string, std::string> tracks;  // original's GUID (upper case) -> its track's GUID
};

std::string TrackGuid(MediaTrack* t)
{
    char buf[64] = {};
    return t && GetSetMediaTrackInfo_String(t, "GUID", buf, false) ? std::string(buf) : std::string();
}

// Every placed item of an Attached library as an instance (its layer, its animation's pool), its look and position.
void ReadWatch(ReaProject* proj, WatchRead* w)
{
    *w = WatchRead();
    am::WatchSnap&                          s = w->snap;
    const SoundStore&                       st = GetSoundStore(proj);
    const std::map<std::string, MediaItem*> items = ItemsByGuid(proj);
    for (const auto& kv : items) s.present.insert(kv.first);
    std::vector<int>                                   pool_of;
    const std::vector<MediaItem*>                      ravs = WithPoolMembers(ProjectRavItems(), proj, &pool_of);
    std::map<std::string, std::pair<MediaItem*, int>> owners;
    for (size_t i = 0; i < ravs.size(); ++i) {
        const std::string g = GuidKey(ItemGuidOf(ravs[i]));
        owners[g] = {ravs[i], i < pool_of.size() ? pool_of[i] : static_cast<int>(i)};
        s.owners[g] = GetMediaItemInfo_Value(ravs[i], "D_POSITION");
    }
    const std::set<std::string>       sources = AllSourceGuids(st, items);
    std::map<std::string, ItemSounds> recs;
    LayerCache                        cache;
    for (const auto& kv : items) {
        PlacedTag t;
        if (!ReadPlacedTag(kv.second, &t) || t.own == Ownership::Deleted) continue;
        const std::string ok = GuidKey(t.owner);
        const auto        o = owners.find(ok);
        if (o == owners.end()) continue;
        auto rec = recs.find(ok);
        if (rec == recs.end()) rec = recs.emplace(ok, ReadItemSounds(o->second.first)).first;
        if (!am::TagListed(rec->second, kv.first)) continue;  // a duplicate / pasted copy: never an instance
        const RuleSounds*   rs = FindRuleSounds(rec->second, t.rid);
        const SoundLibrary* lib = rs && !rs->none ? FindLibrary(st, rs->library) : nullptr;
        if (!lib || !lib->attached) continue;
        const am::LayerAt la = InstanceLayer(st, *lib, t, kv.first, items, cache);
        am::Instance      in;
        in.guid = kv.first;
        in.owner = ok;
        in.pool = ItemSoundName(o->second.first);  // Anim links every item of the same animation (by its name)
        in.rid = t.rid;
        in.t = t.t;
        in.lib = lib->uid;
        in.layer = la.layer;
        in.sig = la.layer.empty() ? std::string() : am::LayerSig(*lib, la.layer);
        in.slot = la.slot;
        s.inst.push_back(in);
        if (sources.count(kv.first) && f_get_chunk) {  // an original: kept as it is, to be put back if deleted by hand
            std::vector<char> buf(1u << 20);
            if (f_get_chunk(kv.second, buf.data(), static_cast<int>(buf.size()), false)) {
                buf.back() = '\0';
                w->chunks[kv.first] = buf.data();
                w->tracks[kv.first] = TrackGuid(GetMediaItem_Track(kv.second));
            }
        }
    }
    for (int i = 0, n = CountSelectedMediaItems(proj); i < n; ++i)
        if (MediaItem* it = GetSelectedMediaItem(proj, i)) s.selected.insert(GuidKey(ItemGuidOf(it)));
}

// An original deleted by hand that a library still plays from: made again from its last read state, untagged, in the
// source zone; a new GUID is given to the library's Sounds. Returns how many.
int BringBackOriginals(ReaProject* proj, const WatchRead& last, const std::set<std::string>& gone)
{
    if (!f_add_item || !f_set_chunk) return 0;
    const std::map<std::string, MediaItem*> items = ItemsByGuid(proj);
    const std::set<std::string>             sources = AllSourceGuids(GetSoundStore(proj), items);
    std::vector<MediaItem*>                 back;
    std::map<std::string, std::string>      renamed;  // old GUID (upper) -> the new item's
    for (const std::string& g : gone) {
        const auto chunk = last.chunks.find(g);
        if (!sources.count(g) || chunk == last.chunks.end() || items.count(g)) continue;
        MediaTrack* track = nullptr;
        for (int i = 0, n = CountTracks(proj); i < n && !track; ++i)
            if (MediaTrack* t = GetTrack(proj, i); t && TrackGuid(t) == last.tracks.at(g)) track = t;
        if (!track) continue;
        MediaItem* it = f_add_item(track);
        if (!it) continue;
        f_set_chunk(it, chunk->second.c_str(), false);
        ClearPlacedTag(it);
        back.push_back(it);
        const std::string now = GuidKey(ItemGuidOf(it));
        if (now != g) renamed[g] = ItemGuidOf(it);
    }
    if (!renamed.empty())
        ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
            bool any = false;
            for (SoundLibrary& l : st.libraries)
                for (SoundNode& n : l.nodes) {
                    const auto hit = renamed.find(GuidKey(n.sound.source.item_guid));
                    if (n.type != SoundNodeType::Sound || hit == renamed.end()) continue;
                    n.sound.source.item_guid = hit->second;
                    any = true;
                }
            return any;
        });
    ToSourceZone(proj, back, {});
    return static_cast<int>(back.size());
}

// Instances deleted by hand (Anim / Project): their layer leaves every linked instance and the library in scope. The
// linked items are deleted, but an item a library Sound still plays from goes to the source zone (an original the user
// deleted is put back there). One RAV undo point right after the user's.
bool PropagateDelete(ReaProject* proj, const WatchRead& last, const std::set<std::string>& gone, am::Scope scope)
{
    const am::DeletePlan dp = am::PlanPropagatedDelete(last.snap.inst, gone, scope);
    if (dp.delete_guids.empty() && dp.removals.empty()) return false;
    const std::string undo = "RAV: Delete linked instances" + ScopeText(scope);
    SoundUndoScope    scope_undo(proj, undo.c_str());
    int               layers = 0;
    for (const am::LayerRemoval& r : dp.removals) {
        MediaItem*          owner = ScanItemByGuid(proj, r.owner);
        const SoundLibrary* lib = owner ? FindLibrary(GetSoundStore(proj), r.lib) : nullptr;
        if (!lib) continue;
        const std::string key = lib->attached_type;
        const std::string kind = KindNameOn(owner, key, lib->name);
        const am::PoolKey pool = PoolKeyOf(owner, kind);
        am::EditOut       edit;
        if (ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
                edit = am::RemoveLayer(st, r.lib, scope, pool, r.layer, r.slot, &NewSoundUid);
                return edit.ok;
            })) {
            ApplyEdit(proj, scope == am::Scope::Anim ? owner : nullptr, r.lib, key, edit);
            ++layers;
        }
    }
    const std::map<std::string, MediaItem*> items = ItemsByGuid(proj);
    const std::set<std::string>             sources = AllSourceGuids(GetSoundStore(proj), items);
    std::map<std::string, std::string>      owner_of;
    for (const am::Instance& i : last.snap.inst) owner_of[i.guid] = i.owner;
    std::vector<MediaItem*> zone;
    int                     deleted = 0;
    for (const std::string& g : dp.delete_guids) {
        const auto hit = items.find(g);
        if (hit == items.end()) continue;
        if (MediaItem* owner = ScanItemByGuid(proj, owner_of[g]))
            ModifyItemSounds(owner, nullptr, [&](ItemSounds& r) { return DetachFromRecord(r, g); });
        if (sources.count(g) || IsFolderItem(hit->second)) {
            ClearPlacedTag(hit->second);
            zone.push_back(hit->second);
        } else if (RemovePlacedAny(proj, hit->second)) {
            ++deleted;
        } else {
            ClearPlacedTag(hit->second);
        }
    }
    ToSourceZone(proj, zone, {});
    const int restored = BringBackOriginals(proj, last, gone);
    UpdateArrange();
    std::string text = undo + ": " + std::to_string(deleted) + (deleted == 1 ? " linked instance deleted" : " linked instances deleted");
    if (!zone.empty() || restored)
        text += ", " + std::to_string(zone.size() + static_cast<size_t>(restored)) + " the library plays from in the source zone";
    if (layers) text += ", the layer left the library";
    Finish(text + ".");
    return true;
}

struct Watch {
    ReaProject*           proj = nullptr;
    bool                  primed = false;
    int                   count = -1;
    std::string           sel_sig;
    bool                  pending = false;    // a change waits to be read (the interval, the mouse button)
    double                next_read = 0.0;    // read at most every 0.25 s
    WatchRead             last;
    std::string           last_top;           // REAPER's last undo point at the last read
    bool                  delete_cmd = false; // a delete action ran since the last read (AttachWatchPostCommand)
};
Watch g_watch;

std::string SelectionSig(ReaProject* proj)
{
    std::string out;
    for (int i = 0, n = CountSelectedMediaItems(proj); i < n; ++i) {
        char p[32];
        std::snprintf(p, sizeof(p), "%p,", static_cast<void*>(GetSelectedMediaItem(proj, i)));
        out += p;
    }
    return out;
}


const std::vector<std::string>& DeleteNames()
{
    static const std::vector<std::string> v =
        LocalizedUndoNames({"Remove items", "Delete items", "Remove items/tracks/envelope points", "Remove selected items"});
    return v;
}

void WatchTick()
{
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj) return;
    if (proj != g_watch.proj) {
        g_watch = Watch();
        g_watch.proj = proj;
    }
    // A drag in progress (the mouse button down): what it did is read after the drop.
    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const SoundStore& st = GetSoundStore(proj);
    bool              any = false;
    for (const SoundLibrary& l : st.libraries) any = any || l.attached;
    if (!any) {
        g_watch.primed = false;
        return;
    }
    const int         count = GetProjectStateChangeCount(proj);
    const std::string sel = SelectionSig(proj);
    const bool        sel_changed = sel != g_watch.sel_sig;
    if (!g_watch.primed || count != g_watch.count || sel_changed) g_watch.pending = true;
    g_watch.count = count;
    g_watch.sel_sig = sel;
    // A selection change is noted at once (no rescan): a delete read right after it knows what was selected.
    if (g_watch.primed && sel_changed) {
        g_watch.last.snap.selected.clear();
        for (int i = 0, n = CountSelectedMediaItems(proj); i < n; ++i)
            if (MediaItem* it = GetSelectedMediaItem(proj, i)) g_watch.last.snap.selected.insert(GuidKey(ItemGuidOf(it)));
    }
    if (!g_watch.pending) return;
    if (down) return;  // a click or a drag in progress: what it did is read after the drop
    const double now_s = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now_s < g_watch.next_read) return;
    g_watch.next_read = now_s + 0.25;
    g_watch.pending = false;
    WatchRead now;
    ReadWatch(proj, &now);
    const char*       top_c = Undo_CanUndo2(proj);
    const std::string top = top_c ? top_c : "";
    if (!g_watch.primed) {
        g_watch.primed = true;
        g_watch.last = now;
        g_watch.last_top = top;
        g_watch.delete_cmd = false;
        return;
    }
    am::WatchIn in;
    in.scope = am::ScopeOf(st);
    in.top = top;
    in.redo = f_can_redo && f_can_redo(proj);
    in.new_undo = top != g_watch.last_top || g_watch.delete_cmd;
    in.cut_names = FollowCutNames();
    in.delete_names = DeleteNames();
    const am::WatchOut out = am::DecideWatch(g_watch.last.snap, now.snap, in);
    bool               wrote = false;
    if (!out.gone.empty()) {
        wrote = PropagateDelete(proj, g_watch.last, out.gone, in.scope);
    }
    if (wrote) {
        ReadWatch(proj, &now);
        const char* t2 = Undo_CanUndo2(proj);
        g_watch.last_top = t2 ? t2 : "";
        g_watch.count = GetProjectStateChangeCount(proj);
        g_watch.sel_sig = SelectionSig(proj);
    } else {
        g_watch.last_top = top;
    }
    g_watch.last = now;
    g_watch.delete_cmd = false;
}

}  // namespace

void InitSoundSpreadSelection(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_set_item = reinterpret_cast<SetMediaItemInfo_ValueFn>(get_func("SetMediaItemInfo_Value"));
    f_count_takes = reinterpret_cast<CountTakesFn>(get_func("CountTakes"));
    f_can_redo = reinterpret_cast<Undo_CanRedo2Fn>(get_func("Undo_CanRedo2"));                    // 13-68e v5
    f_get_chunk = reinterpret_cast<GetItemStateChunkFn>(get_func("GetItemStateChunk"));
    f_set_chunk = reinterpret_cast<SetItemStateChunkFn>(get_func("SetItemStateChunk"));
    f_add_item = reinterpret_cast<AddMediaItemToTrackFn>(get_func("AddMediaItemToTrack"));
}

std::string SpreadSelectionToMarkerType(SpreadScope scope)
{
    try {
        return RunAttach(ScopeOfAction(scope), nullptr);
    } catch (...) {
        return Finish("Attach failed");
    }
}

std::string AddSelectionToStack(SpreadScope scope)
{
    try {
        return RunPile(ScopeOfAction(scope));
    } catch (...) {
        return Finish("Add to pile failed");
    }
}

std::string AttachToMarker(am::Scope scope, const SpreadMarker* marker)
{
    try {
        return RunAttach(scope, marker);
    } catch (...) {
        return Finish("Attach failed");
    }
}

std::string AddToPile(am::Scope scope)
{
    try {
        return RunPile(scope);
    } catch (...) {
        return Finish("Add to pile failed");
    }
}

std::string SeparateLR(am::Scope scope, const SpreadMarker& marker)
{
    try {
        return RunSeparate(scope, marker);
    } catch (...) {
        return Finish("Separate L and R failed");
    }
}

std::string MergeLR(am::Scope scope, const SpreadMarker& marker)
{
    try {
        return RunMerge(scope, marker);
    } catch (...) {
        return Finish("Merge L and R failed");
    }
}

bool SetApplyTo(am::Scope scope)
{
    try {
        const std::string undo = std::string("RAV: Attach manager - Apply to ") + am::ScopeLabel(scope);
        return ModifySoundStore(EnumProjects(-1, nullptr, 0), undo.c_str(), [scope](SoundStore& st) { return am::SetScope(st, scope); });
    } catch (...) {
        return false;
    }
}

void ReadAttachManager(AttachManagerState* out)
{
    if (!out) return;
    try {
        ReadManager(out);
    } catch (...) {
        *out = AttachManagerState();
    }
}

std::string AttachManagerSignature()
{
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    double      a = 0.0, b = 0.0;
    GetSet_LoopTimeRange2(proj, false, false, &a, &b, false);
    char head[96];
    std::snprintf(head, sizeof(head), "%d|%.6f|%.6f|%llu|", GetProjectStateChangeCount(proj), a, b,
                  static_cast<unsigned long long>(SoundStoreGeneration()));
    std::string out = head;
    for (int i = 0, n = CountSelectedMediaItems(proj); i < n; ++i) {
        char p[32];
        std::snprintf(p, sizeof(p), "%p,", static_cast<void*>(GetSelectedMediaItem(proj, i)));
        out += p;
    }
    return out;
}

std::string ClickedMarkersKey()
{
    std::string out;
    for (const std::string& g : g_marker_guids)
        if (ProjectMarkerSelected(g)) out += g + ",";
    return out;
}

void AttachWatchTick()
{
    try {
        WatchTick();
    } catch (...) {
    }
}

void AttachWatchPostCommand(int command, int)
{
    // REAPER's "Item: Remove items" and "Remove items/tracks/envelope points (depending on focus)" (the Delete key):
    // a new delete even when its undo point has the same name as the last one.
    if (command == 40006 || command == 40697) g_watch.delete_cmd = true;
}

}  // namespace rav

#else  // host builds

namespace rav {
std::string SpreadSelectionToMarkerType(SpreadScope) { return {}; }
std::string AddSelectionToStack(SpreadScope) { return {}; }
std::string AttachToMarker(am::Scope, const SpreadMarker*) { return {}; }
std::string AddToPile(am::Scope) { return {}; }
std::string SeparateLR(am::Scope, const SpreadMarker&) { return {}; }
std::string MergeLR(am::Scope, const SpreadMarker&) { return {}; }
bool        SetApplyTo(am::Scope) { return false; }
void        ReadAttachManager(AttachManagerState* out)
{
    if (out) *out = AttachManagerState();
}
std::string AttachManagerSignature() { return {}; }
std::string ClickedMarkersKey() { return {}; }
void        AttachWatchTick() {}
void        AttachWatchPostCommand(int, int) {}
}  // namespace rav

#endif
