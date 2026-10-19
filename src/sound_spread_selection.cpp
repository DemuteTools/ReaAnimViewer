// SPDX-License-Identifier: MIT
//
// See sound_spread_selection.h (13-68).

#include "sound_spread_selection.h"

#ifdef _WIN32

#include <algorithm>
#include <map>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

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
using SelectAllMediaItemsFn = void (*)(ReaProject* proj, bool selected);    // 13-68e
using SetMediaItemSelectedFn = void (*)(MediaItem* item, bool selected);    // 13-68e

SetMediaItemInfo_ValueFn f_set_item = nullptr;
CountTakesFn             f_count_takes = nullptr;
SelectAllMediaItemsFn    f_select_all = nullptr;
SetMediaItemSelectedFn   f_set_selected = nullptr;

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

std::string SideText(char side)
{
    return side ? " " + std::string(1, side) + " only" : std::string();
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
    const SoundLibrary* lib = FindLibrary(GetSoundStore(proj), lib_uid);
    if (!lib || ev.rid.empty()) return;
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

// Populate on `targets`, only the rules holding the library (Antho 2026-10-18: never the project's other
// libraries), then the report as a toast.
// 13-68 v3: both actions count the animations the same way: the items (linked copies included) holding the library.
// 13-68e: `only` (non-null) = only these rules (one side's).
std::string PopulateLibrary(const std::vector<MediaItem*>& targets, const std::string& lib_uid, SpreadRunCounts& counts,
                            const std::set<std::string>* only = nullptr)
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
    if (type_rids.empty()) return Finish("No " + counts.type + " rule holds the library " + counts.type + ": nothing placed");
    PopulateCounts    pc;
    int               events = 0;
    const SoundReport rep = PopulateItemsCounted(targets, &events, &pc, false, nullptr, type_rids);
    counts.events = events;
    counts.placed = pc.placed;
    counts.replaced += pc.replaced;
    counts.kept = pc.kept;
    std::string text = SpreadRunReport(counts);
    for (const ReportLine& l : rep.lines)
        if (l.kind == ReportKind::Blocked) {
            text += "\n" + l.text;
            break;
        }
    UpdateArrange();
    return Finish(text);
}

// 13-68 v3: the library of the tag's rule on its owner ("" when none).
std::string TagLibrary(ReaProject* proj, const PlacedTag& t)
{
    if (t.own == Ownership::Deleted) return std::string();
    MediaItem* o = ScanItemByGuid(proj, t.owner);
    if (!o) return std::string();
    for (const RuleSounds& r : ReadItemSounds(o).rules)
        if (r.rid == t.rid && !r.none) return r.library;
    return std::string();
}

std::string ScopeText(SpreadScope scope)
{
    return scope == SpreadScope::AllInstances ? " (all animations with this marker)" : " (this animation)";
}

// 13-68e: `given` = the Attach manager's marker (else the clicked label); `side_mode` MarkerSide = that marker's side only.
std::string Run(SpreadScope scope, SpreadSide side_mode = SpreadSide::Both, const SpreadMarker* given = nullptr)
{
    if (!f_set_item) return Finish("REAPER functions missing");
    // P3b: no project marker to select (13-68e: the window passes its marker).
    if (!given && GetTaggingMarkerMode() == MarkerMode::Take) return Finish(kTakeOnly);
    ReaProject*                   proj = EnumProjects(-1, nullptr, 0);
    const std::vector<MediaItem*> rav_items = ProjectRavItems();
    // P3: the selected RAV marker.
    SelectedMarker mk;
    int            n_selected = 0;
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
    char       side = 0;  // 13-68e
    {
        ItemRulesRead rr;
        if (ReadItemRules(mk.item, &rr) && rr.valid) {
            kind = KindOfRuleMarker(rr.rules, mk.name, &mk_block);
            if (mk_block >= 0 && side_mode == SpreadSide::MarkerSide) side = EventSide(rr.rules.blocks[static_cast<size_t>(mk_block)]);
        }
    }
    if (kind.key.empty()) return Finish("No rule of this animation writes the marker " + MarkerTypeName(mk.name));
    if (side_mode == SpreadSide::MarkerSide && !side)
        return Finish("The " + MarkerTypeName(mk.name) + " marker has no side: attach it on both sides. Nothing changed");
    const std::string& type = kind.name;
    // The selection (an NVK folder item is one pile).
    std::vector<SelectedSound> sel;
    int                        skipped = 0;
    std::string                skip;
    ReadSelectedSounds(&sel, &skipped, &skip, true, true);
    if (sel.empty()) return Finish("Select the sound items first" + (skip.empty() ? std::string() : " (" + skip + ")"));
    std::vector<AttachItem> facts;
    for (const SelectedSound& s : sel) facts.push_back(s.facts);

    // The animation items: this one (Populate adds its linked copies), or every RAV item with a rule of the kind.
    const bool              all = scope == SpreadScope::AllInstances;
    std::vector<MediaItem*> anims = {mk.item};
    if (all)
        for (MediaItem* it : rav_items) {
            if (it == mk.item) continue;
            ItemRulesRead rr;
            if (!ReadItemRules(it, &rr) || !rr.valid) continue;
            const std::vector<char> on = BlocksOfType(rr.rules, kind.key);
            if (std::find(on.begin(), on.end(), 1) != on.end()) anims.push_back(it);
        }

    // Checks before anything is written: the kind's markers (both sides) on the selected marker's item.
    std::vector<AttachEvent> type_events;
    {
        BusyScope busy("Reading markers");
        MarkersOfType(mk.item, kind.key, &type_events);
    }
    if (type_events.empty()) {
        // A rule of the kind this build no longer runs (an auto block from an older RAV, such as a sideless Land).
        ItemRulesRead rr;
        if (ReadItemRules(mk.item, &rr) && rr.valid)
            for (const Block& b : rr.rules.blocks)
                if (b.enabled && KindOfBlock(b).key == kind.key && IsAutoBlock(b) && !IsActiveAutoBlock(b))
                    return Finish("The " + type + " rule is from an older RAV: press Detect on this animation");
        return Finish("No " + type + " marker on this animation");
    }
    // Antho 2026-10-18 (the concept): the first pile is not copied, it moves onto the selected marker (the event
    // nearest it, of its own rule first: the other side's may sit close) and stays attached there; Populate places
    // the copies on the other markers.
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

    const std::string undo = "RAV: Spread selection to " + type + SideText(side) + ScopeText(scope);
    SoundUndoScope    scope_undo(proj, undo.c_str());

    // The rules of the kind (given ids): the selected marker's item must have one, and its event's rule an id
    // (13-68 v3 review: read before any write; an empty one would leave the original untagged on the marker).
    // 13-68e: "<side> only": that side's rules only (the other side keeps what it had).
    std::vector<std::vector<std::string>> rids_of(anims.size());
    for (size_t i = 0; i < anims.size(); ++i) rids_of[i] = TypeRids(anims[i], kind.key, side);
    if (rids_of[0].empty()) return Finish("Could not give the " + type + " rules an id: nothing changed");
    at_marker.rid = EventRid(mk.item, at_marker);
    if (at_marker.rid.empty()) return Finish("Could not give the " + MarkerTypeName(mk.name) + " rule an id: nothing changed");

    // The kind's Attached library (one for both sides, side-free name): made, or its content replaced.
    SpreadRunCounts counts;
    counts.type = type;
    counts.marker = MarkerTypeName(mk.name);
    counts.all_instances = all;
    counts.selected_markers = n_selected;
    counts.variations = static_cast<int>(piles.size());
    counts.layers = static_cast<int>(sel.size());
    counts.skip_note = skip;
    counts.side = side;  // 13-68e
    std::string lib_uid;
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
            if (side) return SetSideContent(st, lib_uid, side, layer, &NewSoundUid);  // 13-68e: the other side keeps its own
            SetLibraryContent(*lib, layer);
            return true;
        });
        if (!wrote || lib_uid.empty()) return Finish("Could not write the library " + type + ": nothing changed");
    }

    // The assignment: every rule of the kind (both sides), on each animation item and its linked copies.
    for (size_t i = 0; i < anims.size(); ++i) {
        const std::vector<std::string>& of_type = rids_of[i];
        if (of_type.empty()) continue;
        for (MediaItem* m : WithPoolMembers({anims[i]}))
            ModifyItemSounds(m, nullptr, [&](ItemSounds& r) { return SetRulesLibrary(r, of_type, LibState::Library, lib_uid); });
    }

    ApplySideValues(proj, lib_uid);  // 13-68e: each rule plays its side's content
    LeaveOldPlaces(proj, sel);
    // The first pile onto the selected marker (its top item's snap on it), the others to the source zone.
    {
        const double delta = at_marker.project_t - first_anchor;
        for (MediaItem* it : rs.first_items) f_set_item(it, "D_POSITION", GetMediaItemInfo_Value(it, "D_POSITION") + delta);
    }
    ToSourceZone(proj, rs.to_move, rs.first_items);

    const std::vector<MediaItem*> targets = LibraryTargets(anims, lib_uid);
    // 13-68e: "<side> only": the side's rules alone are emptied and placed again.
    const std::set<std::string> side_rids = side ? SideRids(targets, kind.key, side) : std::set<std::string>();
    const std::set<std::string>* only = side ? &side_rids : nullptr;
    // The Spread / Attached sounds already on the events of the library's rules (an earlier spread or attach, or
    // attached by hand) leave them: Populate keeps an event holding one, so it would draw nothing new there. 13-68 v3:
    // never deleted: detached from the record, untagged and moved to the source zone (they may be sources).
    {
        std::set<std::string> moving_guids;
        for (MediaItem* it : rs.to_move) moving_guids.insert(GuidKey(ItemGuidOf(it)));
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
        counts.to_source = static_cast<int>(earlier.size());  // 13-68 v3: the sounds (folder items not counted)
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

    AttachToEvent(proj, mk.item, at_marker, lib_uid, rs.attach_items, 0.0, rs.first_folders);
    return PopulateLibrary(targets, lib_uid, counts, only);
}

// Add to stack (Antho 2026-10-18): no marker to select. The selection holds the new items and one or more items
// already in a stack (the original an Attach left on its marker, or a copy Populate placed: its tag names the event,
// its rule the Attached library), all on one marker (13-68 v3: several markers are refused). The new items are a
// new layer of that library at their offset from the stack's event (top item's snap minus the event: the same
// offset as from the stack item's own snap, which sits on it): all animations = in the shared content; this
// animation = under the library's RAV-owned per-animation Switch (AddAnimationLayer), the stack's animation item
// and its linked copies given that animation's value. The pile nearest the event stays where it is, attached to it;
// the other piles go to the source zone; Populate (the library's rules only) places the new layer on the other markers.
// 13-68e: `side_mode` MarkerSide = the stack's side only (the layer under the library's RAV - Side Switch).
std::string RunStack(SpreadScope scope, SpreadSide side_mode = SpreadSide::Both)
{
    if (!f_set_item) return Finish("REAPER functions missing");
    ReaProject*      proj = EnumProjects(-1, nullptr, 0);
    const SoundStore store = GetSoundStore(proj);
    // The stack items: the selected items placed on an event of an Attached library; the first one's is the stack.
    PlacedTag              ref;
    std::vector<PlacedTag> refs;
    MediaItem*             owner = nullptr;
    std::string            lib_uid, key;
    for (int i = 0, n = CountSelectedMediaItems(proj); i < n; ++i) {
        MediaItem* it = GetSelectedMediaItem(proj, i);
        PlacedTag  t;
        if (!it || !ReadPlacedTag(it, &t)) continue;
        const SoundLibrary* l = FindLibrary(store, TagLibrary(proj, t));
        if (!l || !l->attached || l->attached_type.empty()) continue;
        refs.push_back(t);
        if (owner) continue;
        ref = t;
        owner = ScanItemByGuid(proj, t.owner);
        lib_uid = l->uid;
        key = l->attached_type;
    }
    if (!owner) return Finish("Select one item of the stack (a sound the Attach action placed) with the items to add");
    if (SeveralStackEvents(refs))
        return Finish("The selected stack items are on several markers: select the stack of one marker. Nothing changed");
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    const std::string   type = KindNameOn(owner, key, lib ? lib->name : key);
    // The new items: the selected sounds that are not the stack's (placed for this library: the stack items, the
    // stack's other sounds, its copies on other markers; the attached pile's folder item; the library's own
    // sources); any other one, even one RAV placed, is taken over.
    std::vector<SelectedSound> all_sel, sel;
    int                        skipped = 0;
    std::string                skip;
    ReadSelectedSounds(&all_sel, &skipped, &skip, true, true);
    for (const SelectedSound& s : all_sel) {
        const bool placed_for = s.tagged && TagLibrary(proj, s.tag) == lib_uid;
        bool       kid_in = false;
        if (s.folder)
            for (MediaItem* k : FolderPileMembers(s.item)) {
                PlacedTag kt;
                if (ReadPlacedTag(k, &kt) && TagLibrary(proj, kt) == lib_uid) kid_in = true;
            }
        if (!lib || !IsStackItem(*lib, s.facts.guid, placed_for, kid_in)) sel.push_back(s);
    }
    if (sel.empty()) return Finish("Select the items to add with one item of the stack");

    const bool all = scope == SpreadScope::AllInstances;
    // 13-68e: the stack's side, from its rule (read before the undo point opens).
    char side = 0;
    if (side_mode == SpreadSide::MarkerSide) {
        ItemRulesRead rr;
        if (ReadItemRules(owner, &rr) && rr.valid)
            for (const Block& b : rr.rules.blocks)
                if (!b.rid.empty() && b.rid == ref.rid) side = EventSide(b);
        if (!side) return Finish("The stack's " + type + " marker has no side: add it on both sides. Nothing changed");
    }
    const std::string undo = "RAV: Add selection to stack " + type + SideText(side) + ScopeText(scope);
    // 13-68 v3 review: the scope opens before the rules are given ids (ItemRuleIds may write them).
    SoundUndoScope scope_undo(proj, undo.c_str());
    // The stack's event, in project time.
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
    if (!found) return Finish("The stack's " + type + " marker is not on its animation now: nothing changed");

    std::vector<AttachItem> facts;
    for (const SelectedSound& s : sel) facts.push_back(s.facts);
    const std::vector<std::vector<int>> piles = SpreadPiles(facts);
    const int                           at = NearestPile(facts, piles, ev.project_t);
    const std::vector<int>              first_pile = at >= 0 ? piles[static_cast<size_t>(at)] : std::vector<int>();
    const int                           first_top = TopmostOf(facts, first_pile);
    if (first_top < 0) return Finish("Select the items to add with one item of the stack");
    const double     offset_s = PileAnchor(facts[static_cast<size_t>(first_top)]) - ev.project_t;
    const ReadSounds rs = ReadSources(sel, first_pile);

    SpreadRunCounts counts;
    counts.type = type;
    counts.add_layer = true;
    counts.all_instances = all;
    counts.reused = true;
    counts.variations = static_cast<int>(piles.size());
    counts.layers = static_cast<int>(sel.size());
    counts.skip_note = skip;  // what is not a sound (an animation item...); the stack items are read, then left out
    counts.side = side;       // 13-68e
    // 13-68 v3 review: the values the RAV items outside the stack item's pool hold (another animation's, never reused).
    std::set<std::string> held_elsewhere;
    if (!all) {
        const std::vector<MediaItem*> pool = WithPoolMembers({owner});
        const std::set<MediaItem*>    mine(pool.begin(), pool.end());
        for (MediaItem* it : ProjectRavItems())
            if (!mine.count(it))
                for (const ValueSet& v : ReadItemSounds(it).item_values) held_elsewhere.insert(v.value_uid);
    }
    ValueSet anim_value;
    {
        std::vector<SoundNode> layer = SpreadLayerNodes(rs.sources, type, &NewSoundUid);
        const std::string      anim_name = ItemSoundName(owner);
        const ItemSounds       owner_rec = ReadItemSounds(owner);
        bool                   added = false;
        const bool             wrote = ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
            if (!FindLibrary(st, lib_uid) || layer.empty()) return false;
            // 13-68e: "<side> only": the side's own layers Blend (under the RAV - Side Switch), else the root.
            const std::string top = side ? SideLayerTop(st, lib_uid, side, &NewSoundUid) : std::string();
            if (side && top.empty()) return false;
            SoundLibrary* l = FindLibrary(st, lib_uid);
            if (all) {
                layer[0].name = StackLayerName(*l);
                AddStackLayer(*l, layer, offset_s * 1000.0, NewSoundUid(), top);
                added = true;
            } else {
                layer[0].name = "Layer";
                anim_value = AddAnimationLayer(st, lib_uid, layer, offset_s * 1000.0, anim_name, owner_rec.item_values, type,
                                               &NewSoundUid, held_elsewhere, top);  // 13-68 v3 review; 13-68e: top
                added = !anim_value.group_uid.empty();
            }
            return added;
        });
        if (!wrote || !added) return Finish("Could not write the library " + type + ": nothing changed");
    }
    // This animation: its value of the per-animation Switch, on the item and its linked copies.
    if (!all)
        for (MediaItem* m : WithPoolMembers({owner}))
            ModifyItemSounds(m, nullptr,
                             [&](ItemSounds& r) { return SetValueIn(r.item_values, anim_value.group_uid, anim_value.value_uid); });
    ApplySideValues(proj, lib_uid);  // 13-68e
    LeaveOldPlaces(proj, sel);
    ToSourceZone(proj, rs.to_move, rs.first_items);
    AttachToEvent(proj, owner, ev, lib_uid, rs.attach_items, offset_s, rs.first_folders);
    // All animations: wherever the library is; this animation: the stack's item (Populate adds its linked copies).
    const std::vector<MediaItem*> targets = all ? LibraryTargets({owner}, lib_uid) : std::vector<MediaItem*>{owner};
    const std::set<std::string>   side_rids = side ? SideRids(targets, key, side) : std::set<std::string>();  // 13-68e
    return PopulateLibrary(targets, lib_uid, counts, side ? &side_rids : nullptr);
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

// The items leave: Keep = untagged in place; Delete = deleted, but an item a library Sound plays from goes to the
// source zone (untagged) instead. Counts what happened.
struct Released {
    int kept = 0, deleted = 0, to_source = 0;
};
// `sources` (non-null) = the source GUIDs read before the gesture changed the store (Remove from library: the removed
// variation's original stays the user's item, never deleted).
Released ReleaseItems(ReaProject* proj, const std::vector<std::string>& guids, bool keep,
                      const std::set<std::string>* sources_before = nullptr)
{
    Released                                r;
    const std::map<std::string, MediaItem*> items = ItemsByGuid(proj);
    const std::set<std::string> sources = sources_before ? *sources_before : AllSourceGuids(GetSoundStore(proj), items);
    std::vector<MediaItem*>                 to_zone;
    std::set<std::string>                   done;
    for (const std::string& g : guids) {
        const std::string key = GuidKey(g);
        if (!done.insert(key).second) continue;
        const auto hit = items.find(key);
        if (hit == items.end()) continue;
        MediaItem* it = hit->second;
        if (keep) {
            ClearPlacedTag(it);
            ++r.kept;
        } else if (sources.count(key)) {
            ClearPlacedTag(it);
            to_zone.push_back(it);
            ++r.to_source;
        } else if (IsFolderItem(it) || !RemovePlacedAny(proj, it)) {  // never a folder item (its members would stay)
            ClearPlacedTag(it);
            ++r.kept;
        } else {
            ++r.deleted;
        }
    }
    if (!to_zone.empty()) ToSourceZone(proj, to_zone, {});
    return r;
}

std::string ReleasedText(const Released& r, bool keep)
{
    const int n = r.kept + r.deleted + r.to_source;
    std::string out = std::to_string(n) + (n == 1 ? " item " : " items ");
    if (keep) return out + "kept in place, no longer following";
    out += "deleted";
    if (r.to_source) out += " (" + std::to_string(r.to_source) + " the library plays from: moved to the source zone)";
    return out;
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
    for (int i = 0, n = CountSelectedMediaItems(proj); i < n; ++i) {
        MediaItem* it = GetSelectedMediaItem(proj, i);
        PlacedTag  t;
        if (!it || !ReadPlacedTag(it, &t)) continue;
        const SoundLibrary* l = FindLibrary(store, TagLibrary(proj, t));
        if (!l || !l->attached || l->attached_type.empty()) continue;
        if (stack_lib.empty()) stack_lib = l->uid;
        am::StackRef r;
        r.owner = t.owner;
        r.rid = t.rid;
        r.t = t.t;
        for (size_t k = 0; k < s.events.size(); ++k) {
            const am::AttachedEvent& e = s.events[k];
            if (e.rid == r.rid && std::fabs(e.t - r.t) <= kEventMatchSeconds && e.anim >= 0 &&
                GuidKey(s.anims[static_cast<size_t>(e.anim)].guid) == GuidKey(r.owner))
                r.event = static_cast<int>(k);
        }
        s.stack_refs.push_back(r);
    }
    if (const SoundLibrary* l = stack_lib.empty() ? nullptr : FindLibrary(store, stack_lib))
        for (const SelectedSound& x : sel) {
            const bool placed_for = x.tagged && TagLibrary(proj, x.tag) == stack_lib;
            bool       kid_in = false;
            if (x.folder)
                for (MediaItem* k : FolderPileMembers(x.item)) {
                    PlacedTag kt;
                    if (ReadPlacedTag(k, &kt) && TagLibrary(proj, kt) == stack_lib) kid_in = true;
                }
            if (!IsStackItem(*l, x.facts.guid, placed_for, kid_in)) ++s.fresh;
        }
}

std::string Detach(const std::vector<DetachTarget>& targets, bool keep)
{
    if (targets.empty()) return Finish("Nothing to detach");
    ReaProject*       proj = EnumProjects(-1, nullptr, 0);
    const SoundStore  store = GetSoundStore(proj);
    const std::string where = targets.size() > 1 ? std::to_string(targets.size()) + " markers"
                                                 : (targets.front().label.empty() ? std::string("the marker") : targets.front().label);
    const std::string undo = "RAV: Detach from " + where;
    SoundUndoScope    scope_undo(proj, undo.c_str());
    std::vector<std::string> released;
    int                      events = 0;
    for (const DetachTarget& t : targets) {
        MediaItem* owner = ScanItemByGuid(proj, t.owner);
        if (!owner) continue;
        const ItemSounds    rec = ReadItemSounds(owner);
        const RuleSounds*   rs = FindRuleSounds(rec, t.rid);
        const SoundLibrary* lib = rs && !rs->none ? FindLibrary(store, rs->library) : nullptr;
        if (!lib) continue;
        ModifyItemSounds(owner, nullptr, [&](ItemSounds& r) {
            EventSounds* e = FindEventSounds(r, t.rid, t.t);
            if (!e) return false;
            const std::vector<std::string> g = am::DetachEventInRecord(*e, *lib);
            if (g.empty()) return false;  // nothing live there: unchanged, not counted
            released.insert(released.end(), g.begin(), g.end());
            ++events;
            return true;
        });
    }
    if (events == 0) return Finish(undo + ": nothing attached there now. Nothing changed");
    const Released r = ReleaseItems(proj, released, keep);
    UpdateArrange();
    return Finish(undo + ": " + ReleasedText(r, keep) + "; " + (events > 1 ? "those markers stay" : "the marker stays") + " empty.");
}

std::string RemoveVariation(const std::string& lib_uid, const std::string& node, bool keep)
{
    ReaProject*         proj = EnumProjects(-1, nullptr, 0);
    const SoundStore    store = GetSoundStore(proj);
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    const SoundNode*    n = lib ? FindNode(*lib, node) : nullptr;
    if (!lib || !n) return Finish("That variation is no longer in the library: nothing changed");
    const std::string name = n->name.empty() ? std::string("the variation") : n->name;
    const std::string lib_name = lib->name;
    const std::map<std::string, MediaItem*>  all_items = ItemsByGuid(proj);
    const std::map<std::string, std::string> node_of = SourceNodes(*lib, all_items);
    const std::set<std::string>              sources = AllSourceGuids(store, all_items);  // before the variation leaves
    const std::string undo = "RAV: Remove " + name + " from the " + lib_name + " library";
    SoundUndoScope    scope_undo(proj, undo.c_str());
    am::RemovedVariation rv;
    int                  left = 0;
    const bool           wrote = ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
        SoundLibrary* l = FindLibrary(st, lib_uid);
        if (!l || !am::RemoveVariationFromLibrary(*l, node, &rv)) return false;
        left = rv.random.empty() ? 0 : static_cast<int>(ChildrenOf(*l, rv.random).size());
        return true;
    });
    if (!wrote) return Finish("Could not write the library " + lib_name + ": nothing changed");
    std::vector<std::string> released;
    std::vector<MediaItem*>  owners;
    std::set<std::string>    lib_rids;
    int                      events = 0;
    for (MediaItem* it : ProjectRavItems()) {
        const ItemSounds      before = ReadItemSounds(it);
        std::set<std::string> rids;
        for (const RuleSounds& r : before.rules)
            if (r.library == lib_uid && !r.none && !r.rid.empty()) rids.insert(r.rid);
        if (rids.empty()) continue;
        lib_rids.insert(rids.begin(), rids.end());
        const std::string was = SerializeItemSounds(before);
        int               here = 0;
        ModifyItemSounds(it, nullptr, [&](ItemSounds& r) {
            const std::vector<std::string> g = am::RemoveVariationInRecord(r, rids, rv, keep, node_of, &here);
            released.insert(released.end(), g.begin(), g.end());
            return SerializeItemSounds(r) != was;
        });
        events += here;
        if (here > 0) owners.push_back(it);
    }
    const Released r = ReleaseItems(proj, released, keep, &sources);
    std::string    text = undo + ": " + ReleasedText(r, keep);
    if (!keep && !owners.empty()) {
        if (!rv.random.empty() && left > 0) {
            int            n_events = 0;
            PopulateCounts pc;
            PopulateItemsCounted(owners, &n_events, &pc, false, nullptr, std::vector<std::string>(lib_rids.begin(), lib_rids.end()));
            text += ", " + std::to_string(events) + (events == 1 ? " marker" : " markers") + " drawn again from the " +
                    std::to_string(left) + (left == 1 ? " variation left" : " variations left");
        } else if (!rv.random.empty()) {
            text += ", the library is now empty: those markers stay empty";
        }
    }
    UpdateArrange();
    return Finish(text + ".");
}

int SelectItems(const std::vector<std::string>& guids)
{
    if (!f_select_all || !f_set_selected) return 0;
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    const std::map<std::string, MediaItem*> items = ItemsByGuid(proj);
    f_select_all(proj, false);
    int n = 0;
    for (const std::string& g : guids) {
        const auto hit = items.find(GuidKey(g));
        if (hit == items.end()) continue;
        f_set_selected(hit->second, true);
        ++n;
    }
    UpdateArrange();
    return n;
}

}  // namespace

void InitSoundSpreadSelection(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_set_item = reinterpret_cast<SetMediaItemInfo_ValueFn>(get_func("SetMediaItemInfo_Value"));
    f_count_takes = reinterpret_cast<CountTakesFn>(get_func("CountTakes"));
    f_select_all = reinterpret_cast<SelectAllMediaItemsFn>(get_func("SelectAllMediaItems"));      // 13-68e
    f_set_selected = reinterpret_cast<SetMediaItemSelectedFn>(get_func("SetMediaItemSelected"));  // 13-68e
}

std::string SpreadSelectionToMarkerType(SpreadScope scope)
{
    try {
        return Run(scope);
    } catch (...) {
        return Finish("Spread selection failed");
    }
}

std::string AddSelectionToStack(SpreadScope scope)
{
    try {
        return RunStack(scope);
    } catch (...) {
        return Finish("Add to stack failed");
    }
}

std::string SpreadSelectionToMarker(SpreadScope scope, SpreadSide side, const SpreadMarker* marker)
{
    try {
        return Run(scope, side, marker);
    } catch (...) {
        return Finish("Attach failed");
    }
}

std::string AddSelectionToStackSide(SpreadScope scope, SpreadSide side)
{
    try {
        return RunStack(scope, side);
    } catch (...) {
        return Finish("Add to stack failed");
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

std::string DetachFromMarkers(const std::vector<DetachTarget>& targets, bool keep)
{
    try {
        return Detach(targets, keep);
    } catch (...) {
        return Finish("Detach failed");
    }
}

std::string RemoveFromAttachedLibrary(const std::string& lib_uid, const std::string& node, bool keep)
{
    try {
        return RemoveVariation(lib_uid, node, keep);
    } catch (...) {
        return Finish("Remove from library failed");
    }
}

int SelectItemsByGuid(const std::vector<std::string>& guids)
{
    try {
        return SelectItems(guids);
    } catch (...) {
        return 0;
    }
}

}  // namespace rav

#else  // host builds

namespace rav {
std::string SpreadSelectionToMarkerType(SpreadScope) { return {}; }
std::string AddSelectionToStack(SpreadScope) { return {}; }
std::string SpreadSelectionToMarker(SpreadScope, SpreadSide, const SpreadMarker*) { return {}; }
std::string AddSelectionToStackSide(SpreadScope, SpreadSide) { return {}; }
void        ReadAttachManager(AttachManagerState* out)
{
    if (out) *out = AttachManagerState();
}
std::string AttachManagerSignature() { return {}; }
std::string ClickedMarkersKey() { return {}; }
std::string DetachFromMarkers(const std::vector<DetachTarget>&, bool) { return {}; }
std::string RemoveFromAttachedLibrary(const std::string&, const std::string&, bool) { return {}; }
int         SelectItemsByGuid(const std::vector<std::string>&) { return 0; }
}  // namespace rav

#endif
