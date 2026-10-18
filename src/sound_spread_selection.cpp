// SPDX-License-Identifier: MIT
//
// See sound_spread_selection.h (13-68).

#include "sound_spread_selection.h"

#ifdef _WIN32

#include <algorithm>
#include <map>
#include <cctype>
#include <cmath>
#include <set>
#include <vector>

#include "auto_detect.h"  // IsActiveAutoBlock
#include "bone_events.h"  // IsAutoBlock
#include "busy_progress_ui.h"
#include "console_log.h"
#include "event_list.h"
#include "item_rules.h"
#include "reaper_api.h"
#include "reaper_item_read.h"
#include "reaper_toast.h"
#include "rule_record.h"
#include "sound_attach.h"
#include "sound_attach_plan.h"
#include "sound_auto_assign.h"  // SetRulesLibrary
#include "sound_peaks.h"        // ReadItemMedia
#include "sound_place.h"        // FolderPileMembers
#include "sound_populate.h"
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

// The blocks of `rules` of the marker type `type` that are on.
std::vector<char> BlocksOfType(const ItemRules& rules, const std::string& type)
{
    std::vector<char> out(rules.blocks.size(), 0);
    for (size_t b = 0; b < rules.blocks.size(); ++b)
        out[b] = rules.blocks[b].enabled && SameMarkerType(rules.blocks[b].marker, type) ? 1 : 0;
    return out;
}

// The type of a marker named `name` on an item with these rules (TypeOfMarker).
std::string TypeOfRuleMarker(const ItemRules& rules, const std::string& name)
{
    std::vector<std::string> markers;
    for (const Block& b : rules.blocks) markers.push_back(b.marker);
    return TypeOfMarker(markers, name);
}

// The markers of the type on `item` (DetectItem -> BuildEventList -> AttachEventsOf). False when the
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
std::vector<std::string> TypeRids(MediaItem* anim, const std::string& type)
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
        if (on[b] && !rids[b].empty()) out.push_back(rids[b]);
    return out;
}

// The selection read as the library reads it (an NVK folder item: its pile, its layers' takes shared), and the
// items each pile moves: `first` = the pile that is not copied (it is attached to an event).
struct ReadSounds {
    std::vector<SpreadSource> sources;
    std::vector<MediaItem*>   to_move;       // the originals and the folder piles' child items
    std::set<MediaItem*>      first_items;   // the first pile's (and its folder pile's children)
    std::vector<MediaItem*>   attach_items;  // those of them that sound (a folder item's children, not it)
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

// The selected items become the library's sources: an attached one leaves its event, none keeps a RAV tag.
void LeaveOldPlaces(ReaProject* proj, const std::vector<SelectedSound>& sel)
{
    for (const SelectedSound& s : sel) {
        if (!s.tagged) continue;
        if (s.tag.own == Ownership::Attached)
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
// avoid-repeat counts it.
void AttachToEvent(ReaProject* proj, MediaItem* owner, const AttachEvent& ev, const std::string& lib_uid,
                   const std::vector<MediaItem*>& items, double offset_s)
{
    const SoundLibrary* lib = FindLibrary(GetSoundStore(proj), lib_uid);
    if (!lib || ev.rid.empty()) return;
    std::vector<SoundPlacement>              placements;
    std::vector<std::pair<std::string, int>> played;
    for (MediaItem* it : items) {
        ItemFacts f;
        if (!ReadItemFacts(it, &f)) continue;
        int v = -1;
        for (const SoundNode& n : lib->nodes)
            if (n.type == SoundNodeType::Sound && SameGuid(n.sound.source.item_guid, f.guid)) v = PlayedVariation(n.sound, it);
        played.push_back({f.guid, v});
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
std::string PopulateLibrary(const std::vector<MediaItem*>& targets, const std::string& lib_uid, SpreadRunCounts& counts)
{
    std::vector<std::string> type_rids;
    for (MediaItem* m : WithPoolMembers(targets))
        for (const RuleSounds& r : ReadItemSounds(m).rules)
            if (r.library == lib_uid && !r.none && !r.rid.empty() &&
                std::find(type_rids.begin(), type_rids.end(), r.rid) == type_rids.end())
                type_rids.push_back(r.rid);
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

std::string Run(SpreadScope scope)
{
    if (!f_set_item) return Finish("REAPER functions missing");
    // P3b: no project marker to select.
    if (GetTaggingMarkerMode() == MarkerMode::Take) return Finish(kTakeOnly);
    ReaProject*                   proj = EnumProjects(-1, nullptr, 0);
    const std::vector<MediaItem*> rav_items = ProjectRavItems();
    // P3: the selected RAV marker.
    SelectedMarker mk;
    const int      n_selected = FindSelectedMarker(rav_items, &mk);
    if (n_selected == 0 || !mk.item) return Finish(kNoMarker);
    std::string type;
    {
        ItemRulesRead rr;
        if (ReadItemRules(mk.item, &rr) && rr.valid) type = TypeOfRuleMarker(rr.rules, mk.name);
    }
    if (type.empty()) return Finish("No rule of this animation writes the marker " + MarkerTypeName(mk.name));
    // The selection (an NVK folder item is one pile).
    std::vector<SelectedSound> sel;
    int                        skipped = 0;
    std::string                skip;
    ReadSelectedSounds(&sel, &skipped, &skip, true);
    if (sel.empty()) return Finish("Select the sound items first" + (skip.empty() ? std::string() : " (" + skip + ")"));
    std::vector<AttachItem> facts;
    for (const SelectedSound& s : sel) facts.push_back(s.facts);

    // The animation items: this one (Populate adds its linked copies), or every RAV item with a rule of the type.
    const bool              all = scope == SpreadScope::AllInstances;
    std::vector<MediaItem*> anims = {mk.item};
    if (all)
        for (MediaItem* it : rav_items) {
            if (it == mk.item) continue;
            ItemRulesRead rr;
            if (!ReadItemRules(it, &rr) || !rr.valid) continue;
            const std::vector<char> on = BlocksOfType(rr.rules, type);
            if (std::find(on.begin(), on.end(), 1) != on.end()) anims.push_back(it);
        }

    // Checks before anything is written: the type's markers on the selected marker's item.
    std::vector<AttachEvent> type_events;
    {
        BusyScope busy("Reading markers");
        MarkersOfType(mk.item, type, &type_events);
    }
    if (type_events.empty()) {
        // A rule of the type this build no longer runs (an auto block from an older RAV, such as a sideless Land).
        ItemRulesRead rr;
        if (ReadItemRules(mk.item, &rr) && rr.valid)
            for (const Block& b : rr.rules.blocks)
                if (b.enabled && SameMarkerType(b.marker, type) && IsAutoBlock(b) && !IsActiveAutoBlock(b))
                    return Finish("The " + type + " rule is from an older RAV: press Detect on this animation");
        return Finish("No " + type + " marker on this animation");
    }
    // Antho 2026-10-18 (the concept): the first pile is not copied, it moves onto the selected marker (the event
    // nearest it) and stays attached there; Populate places the copies on the other markers.
    AttachEvent at_marker = type_events.front();
    for (const AttachEvent& e : type_events)
        if (std::fabs(e.project_t - mk.pos) < std::fabs(at_marker.project_t - mk.pos)) at_marker = e;
    const std::vector<std::vector<int>> piles = SpreadPiles(facts);
    const std::vector<int>              first_pile = piles.empty() ? std::vector<int>() : piles.front();
    const int                           first_top = TopmostOf(facts, first_pile);
    const double first_anchor = first_top >= 0 ? PileAnchor(facts[static_cast<size_t>(first_top)]) : at_marker.project_t;
    const ReadSounds rs = ReadSources(sel, first_pile);

    const std::string undo = "RAV: Spread selection to " + type + (all ? " (all instances)" : " (this animation)");
    SoundUndoScope    scope_undo(proj, undo.c_str());

    // The rules of the type (given ids): the selected marker's item must have one.
    std::vector<std::vector<std::string>> rids_of(anims.size());
    for (size_t i = 0; i < anims.size(); ++i) rids_of[i] = TypeRids(anims[i], type);
    if (rids_of[0].empty()) return Finish("Could not give the " + type + " rules an id: nothing changed");

    // The type's Attached library: made, or its content replaced.
    SpreadRunCounts counts;
    counts.type = type;
    counts.all_instances = all;
    counts.selected_markers = n_selected;
    counts.variations = static_cast<int>(piles.size());
    counts.layers = static_cast<int>(sel.size());
    counts.skip_note = skip;
    std::string lib_uid;
    {
        const std::vector<SoundNode> layer = SpreadLayerNodes(rs.sources, type, &NewSoundUid);
        const bool                   wrote = ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
            SoundLibrary* lib = FindAttachedLibrary(st, type);
            counts.reused = lib != nullptr;
            if (!lib) {
                SoundLibrary l;
                l.uid = NewSoundUid();
                l.name = type;
                l.attached = true;
                st.libraries.push_back(l);
                lib = &st.libraries.back();
            }
            lib->attached_type = type;  // found by it from now on, whatever its name
            SetLibraryContent(*lib, layer);
            lib_uid = lib->uid;
            return true;
        });
        if (!wrote || lib_uid.empty()) return Finish("Could not write the library " + type + ": nothing changed");
    }

    // The assignment: every rule writing the type, on each animation item and its linked copies.
    std::set<MediaItem*> assigned;
    for (size_t i = 0; i < anims.size(); ++i) {
        const std::vector<std::string>& of_type = rids_of[i];
        if (of_type.empty()) continue;
        for (MediaItem* m : WithPoolMembers({anims[i]})) {
            ModifyItemSounds(m, nullptr, [&](ItemSounds& r) { return SetRulesLibrary(r, of_type, LibState::Library, lib_uid); });
            assigned.insert(m);
        }
    }
    counts.animations = static_cast<int>(assigned.size());

    LeaveOldPlaces(proj, sel);
    // The first pile onto the selected marker (its top item's snap on it), the others to the source zone.
    {
        const double delta = at_marker.project_t - first_anchor;
        for (MediaItem* it : rs.first_items) f_set_item(it, "D_POSITION", GetMediaItemInfo_Value(it, "D_POSITION") + delta);
    }
    ToSourceZone(proj, rs.to_move, rs.first_items);

    const std::vector<MediaItem*> targets = LibraryTargets(anims, lib_uid);
    // The Spread / Attached sounds already on the events of the library's rules go first (an earlier spread or
    // attach): Populate keeps an event holding one, so it would draw nothing new there. The library's content was
    // replaced: they are no longer its sounds.
    {
        std::set<std::string> moving_guids;
        for (MediaItem* it : rs.to_move) moving_guids.insert(GuidKey(ItemGuidOf(it)));
        std::map<std::string, MediaItem*> by_guid;
        for (int i = 0, n = CountMediaItems(proj); i < n; ++i)
            if (MediaItem* it = GetMediaItem(proj, i)) by_guid[GuidKey(ItemGuidOf(it))] = it;
        for (MediaItem* m : WithPoolMembers(targets)) {
            ItemSounds            rec = ReadItemSounds(m);
            std::set<std::string> rids;
            for (const RuleSounds& r : rec.rules)
                if (r.library == lib_uid && !r.none) rids.insert(r.rid);
            if (rids.empty()) continue;
            std::vector<std::string> drop;
            for (const EventSounds& e : rec.events)
                if (rids.count(e.rid))
                    for (const SoundPlacement& p : e.placed)
                        if (p.own == Ownership::Attached || p.own == Ownership::Spread) drop.push_back(p.item_guid);
            if (drop.empty()) continue;
            for (const std::string& g : drop) {
                const std::string key = GuidKey(g);
                if (!moving_guids.count(key)) {
                    auto hit = by_guid.find(key);
                    if (hit != by_guid.end() && RemovePlacedAny(proj, hit->second)) {
                        by_guid.erase(hit);
                        ++counts.replaced;
                    }
                }
            }
            ModifyItemSounds(m, nullptr, [&drop](ItemSounds& r) {
                bool any = false;
                for (const std::string& g : drop) any = DetachFromRecord(r, g) || any;
                return any;
            });
        }
    }

    at_marker.rid = EventRid(mk.item, at_marker);
    AttachToEvent(proj, mk.item, at_marker, lib_uid, rs.attach_items, 0.0);
    return PopulateLibrary(targets, lib_uid, counts);
}

// Add to stack (Antho 2026-10-18): no marker to select. The selection holds one item already in a stack (the
// original an Attach left on its marker, or a copy Populate placed: its tag names the event, its rule the Attached
// library) and the new items, put by the user where they go against it. The new items are a new layer of that
// library at their offset from the stack's event (top item's snap minus the event: the same offset as from the
// stack item's own snap, which sits on it); the pile nearest the event stays where it is, attached to it; the
// other piles go to the source zone; Populate (the library's rules only) places the new layer on the other markers.
std::string RunStack()
{
    if (!f_set_item) return Finish("REAPER functions missing");
    ReaProject*      proj = EnumProjects(-1, nullptr, 0);
    const SoundStore store = GetSoundStore(proj);
    // The stack item: the first selected item placed on an event of an Attached library.
    PlacedTag   ref;
    MediaItem*  owner = nullptr;
    std::string lib_uid, type;
    for (int i = 0, n = CountSelectedMediaItems(proj); i < n && !owner; ++i) {
        MediaItem* it = GetSelectedMediaItem(proj, i);
        PlacedTag  t;
        if (!it || !ReadPlacedTag(it, &t) || t.own == Ownership::Deleted) continue;
        MediaItem* o = ScanItemByGuid(proj, t.owner);
        if (!o) continue;
        for (const RuleSounds& r : ReadItemSounds(o).rules) {
            if (r.rid != t.rid || r.none) continue;
            const SoundLibrary* l = FindLibrary(store, r.library);
            if (!l || !l->attached || l->attached_type.empty()) continue;
            ref = t;
            owner = o;
            lib_uid = l->uid;
            type = l->attached_type;
        }
    }
    if (!owner) return Finish("Select one item of the stack (a sound the Attach action placed) with the items to add");
    // The new items: the selected sounds not already on that event.
    std::vector<SelectedSound> all_sel, sel;
    int                        skipped = 0;
    std::string                skip;
    ReadSelectedSounds(&all_sel, &skipped, &skip, true);
    for (const SelectedSound& s : all_sel)
        if (!(s.tagged && SameGuid(s.tag.owner, ref.owner) && s.tag.rid == ref.rid &&
              std::fabs(s.tag.t - ref.t) <= kEventMatchSeconds))
            sel.push_back(s);
    if (sel.empty()) return Finish("Select the items to add with one item of the stack");
    // The stack's event, in project time.
    AttachEvent ev;
    bool        found = false;
    {
        BusyScope                busy("Reading markers");
        std::vector<std::string> rids;
        std::string              err;
        std::vector<AttachEvent> evs;
        if (ItemRuleIds(owner, &rids, &err) && MarkersOfType(owner, type, &evs))
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

    SoundUndoScope scope_undo(proj, ("RAV: Add selection to stack " + type).c_str());
    SpreadRunCounts counts;
    counts.type = type;
    counts.add_layer = true;
    counts.reused = true;
    counts.variations = static_cast<int>(piles.size());
    counts.layers = static_cast<int>(sel.size());
    {  // no skip note: the stack item, when a placed copy, is skipped as placed by RAV
        std::vector<SoundNode> layer = SpreadLayerNodes(rs.sources, type, &NewSoundUid);
        bool                   added = false;
        const bool             wrote = ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
            SoundLibrary* lib = FindLibrary(st, lib_uid);
            if (!lib || layer.empty()) return false;
            layer[0].name = StackLayerName(*lib);
            AddStackLayer(*lib, layer, offset_s * 1000.0, NewSoundUid());
            added = true;
            return true;
        });
        if (!wrote || !added) return Finish("Could not write the library " + type + ": nothing changed");
    }
    LeaveOldPlaces(proj, sel);
    ToSourceZone(proj, rs.to_move, rs.first_items);
    AttachToEvent(proj, owner, ev, lib_uid, rs.attach_items, offset_s);
    const std::vector<MediaItem*> targets = LibraryTargets({owner}, lib_uid);
    counts.animations = static_cast<int>(WithPoolMembers(targets).size());
    return PopulateLibrary(targets, lib_uid, counts);
}

}  // namespace

void InitSoundSpreadSelection(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_set_item = reinterpret_cast<SetMediaItemInfo_ValueFn>(get_func("SetMediaItemInfo_Value"));
    f_count_takes = reinterpret_cast<CountTakesFn>(get_func("CountTakes"));
}

std::string SpreadSelectionToMarkerType(SpreadScope scope)
{
    try {
        return Run(scope);
    } catch (...) {
        return Finish("Spread selection failed");
    }
}

std::string AddSelectionToStack()
{
    try {
        return RunStack();
    } catch (...) {
        return Finish("Add to stack failed");
    }
}

}  // namespace rav

#else  // host builds

namespace rav {
std::string SpreadSelectionToMarkerType(SpreadScope) { return {}; }
std::string AddSelectionToStack() { return {}; }
}  // namespace rav

#endif
