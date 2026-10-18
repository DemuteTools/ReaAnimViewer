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

std::string Run(SpreadScope scope, bool add_layer)
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
    std::vector<int> every;
    for (size_t i = 0; i < facts.size(); ++i) every.push_back(static_cast<int>(i));
    const int top = TopmostOf(facts, every);

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

    // Checks before anything is written: the type's markers on the selected marker's item; Add to stack: the
    // new layer keeps the offset the selection's top item has to its nearest one (no Attached library for the
    // type yet: a spread).
    std::vector<double>      marker_times;
    std::vector<AttachEvent> type_events;
    {
        BusyScope busy("Reading markers");
        if (MarkersOfType(mk.item, type, &type_events))
            for (const AttachEvent& e : type_events) marker_times.push_back(e.project_t);
    }
    if (marker_times.empty()) {
        // A rule of the type this build no longer runs (an auto block from an older RAV, such as a sideless Land).
        ItemRulesRead rr;
        if (ReadItemRules(mk.item, &rr) && rr.valid)
            for (const Block& b : rr.rules.blocks)
                if (b.enabled && SameMarkerType(b.marker, type) && IsAutoBlock(b) && !IsActiveAutoBlock(b))
                    return Finish("The " + type + " rule is from an older RAV: press Detect on this animation");
        return Finish("No " + type + " marker on this animation");
    }
    const bool   stack = add_layer && FindAttachedLibrary(GetSoundStore(proj), type) != nullptr;
    const double layer_offset = stack ? StackLayerOffset(PileAnchor(facts[static_cast<size_t>(top)]), marker_times) : 0.0;
    // Antho 2026-10-18 (the concept): the first pile is not copied, it moves onto the selected marker (the event
    // nearest it) and stays attached there; Populate places the copies on the other markers.
    AttachEvent at_marker = type_events.front();
    for (const AttachEvent& e : type_events)
        if (std::fabs(e.project_t - mk.pos) < std::fabs(at_marker.project_t - mk.pos)) at_marker = e;
    const std::vector<std::vector<int>> piles = SpreadPiles(facts);
    const std::vector<int>              first_pile = piles.empty() ? std::vector<int>() : piles.front();
    const int                           first_top = TopmostOf(facts, first_pile);
    const double first_anchor = first_top >= 0 ? PileAnchor(facts[static_cast<size_t>(first_top)]) : at_marker.project_t;

    // The sounds as the library reads them (an NVK folder item: its pile, its layers' takes shared).
    std::vector<SpreadSource>  sources;
    std::vector<MediaItem*>    to_move;  // the originals and the folder piles' child items
    std::set<MediaItem*>       moving;
    std::set<MediaItem*>       on_marker;       // the first pile's items (and a folder pile's children)
    std::vector<MediaItem*>    attach_items;    // those that sound (a folder item's children, not it)
    for (size_t si = 0; si < sel.size(); ++si) {
        const SelectedSound& s = sel[si];
        const bool first = std::find(first_pile.begin(), first_pile.end(), static_cast<int>(si)) != first_pile.end();
        SpreadSource src;
        src.facts = s.facts;
        src.name = ItemSoundName(s.item);
        src.folder = s.folder;
        if (s.folder) {
            const std::vector<MediaItem*> kids = FolderPileMembers(s.item);
            int                           takes = 0;
            for (MediaItem* k : kids) {
                takes = std::max(takes, f_count_takes ? f_count_takes(k) : 1);
                if (moving.insert(k).second) to_move.push_back(k);
                if (first && on_marker.insert(k).second) attach_items.push_back(k);
            }
            SoundMedia top_media;  // take markers (NVK): the top member's slices are the pile's variations
            if (!kids.empty()) ReadItemMedia(kids.front(), &top_media);
            src.sound = PileSoundData(s.facts.guid, takes, s.facts.len, top_media.facts.slices);
        } else {
            src.sound = ItemSoundData(s.item);
        }
        if (moving.insert(s.item).second) to_move.push_back(s.item);
        if (first && on_marker.insert(s.item).second && !s.folder) attach_items.push_back(s.item);
        sources.push_back(src);
    }

    const std::string undo = (add_layer ? "RAV: Add selection to stack on " : "RAV: Spread selection to ") + type
                           + (all ? " (all instances)" : " (this animation)");
    SoundUndoScope scope_undo(proj, undo.c_str());

    // The rules of the type (given ids): the selected marker's item must have one.
    std::vector<std::vector<std::string>> rids_of(anims.size());
    for (size_t i = 0; i < anims.size(); ++i) rids_of[i] = TypeRids(anims[i], type);
    if (rids_of[0].empty()) return Finish("Could not give the " + type + " rules an id: nothing changed");

    // The type's Attached library: made, its content replaced (spread), or a layer added (Add to stack).
    SpreadRunCounts counts;
    counts.type = type;
    counts.all_instances = all;
    counts.add_layer = stack;
    counts.selected_markers = n_selected;
    counts.variations = static_cast<int>(SpreadPiles(facts).size());
    counts.layers = static_cast<int>(sel.size());
    counts.skip_note = skip;
    std::string lib_uid;
    {
        std::vector<SoundNode> layer = SpreadLayerNodes(sources, type, &NewSoundUid);
        const bool             wrote = ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
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
            if (stack) {
                if (!layer.empty()) layer[0].name = StackLayerName(*lib);
                AddStackLayer(*lib, layer, layer_offset * 1000.0, NewSoundUid());
            } else {
                SetLibraryContent(*lib, layer);
            }
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

    // The originals become the library's sources: an attached one leaves its event, none keeps a RAV tag.
    std::set<std::string> moving_guids;
    for (MediaItem* it : to_move) moving_guids.insert(GuidKey(ItemGuidOf(it)));
    for (const SelectedSound& s : sel) {
        if (!s.tagged) continue;
        if (s.tag.own == Ownership::Attached)
            if (MediaItem* owner = ScanItemByGuid(proj, s.tag.owner)) {
                const std::string g = s.facts.guid;
                ModifyItemSounds(owner, nullptr, [&g](ItemSounds& r) { return DetachFromRecord(r, g); });
            }
        ClearPlacedTag(s.item);
    }
    // The first pile onto the selected marker (its top item's snap on it; Add to stack: at the layer's offset).
    {
        const double delta = at_marker.project_t + layer_offset - first_anchor;
        for (MediaItem* it : on_marker) f_set_item(it, "D_POSITION", GetMediaItemInfo_Value(it, "D_POSITION") + delta);
    }
    // The other piles to the source zone: after the project end, their relative positions kept.
    {
        double    project_end = 0.0;
        const int n = CountMediaItems(proj);
        for (int i = 0; i < n; ++i) {
            MediaItem* it = GetMediaItem(proj, i);
            if (!it || moving_guids.count(GuidKey(ItemGuidOf(it)))) continue;
            project_end = std::max(project_end, GetMediaItemInfo_Value(it, "D_POSITION") + GetMediaItemInfo_Value(it, "D_LENGTH"));
        }
        std::vector<MediaItem*> zone;
        std::vector<double>     from;
        for (MediaItem* it : to_move)
            if (!on_marker.count(it)) {
                zone.push_back(it);
                from.push_back(GetMediaItemInfo_Value(it, "D_POSITION"));
            }
        const std::vector<double> to = SourceZonePositions(project_end, from);
        for (size_t i = 0; i < zone.size() && i < to.size(); ++i) f_set_item(zone[i], "D_POSITION", to[i]);
    }

    // Populate places it (draws, avoid-repeat, takes, Blend layers, loops, linked copies, follow) on the
    // targets and on every other animation item the library is assigned to (an earlier run's).
    std::vector<MediaItem*> targets = anims;
    for (MediaItem* it : rav_items) {
        if (std::find(targets.begin(), targets.end(), it) != targets.end()) continue;
        const ItemSounds rec = ReadItemSounds(it);
        for (const RuleSounds& r : rec.rules)
            if (r.library == lib_uid && !r.none) {
                targets.push_back(it);
                break;
            }
    }
    // The Spread / Attached sounds already on the events of the library's rules go first (an earlier spread or
    // attach): Populate keeps an event holding one, so it would draw nothing new there.
    {
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

    // The first pile attached to the selected marker's event (13-10's record and tag): it follows that event and
    // Populate leaves it alone; its draws feed the avoid-repeat of the copies.
    {
        std::vector<std::string> all_rids;
        std::string              err;
        if (ItemRuleIds(mk.item, &all_rids, &err) && at_marker.block >= 0 && static_cast<size_t>(at_marker.block) < all_rids.size())
            at_marker.rid = all_rids[static_cast<size_t>(at_marker.block)];
        const SoundLibrary* lib = FindLibrary(GetSoundStore(proj), lib_uid);
        if (at_marker.rid.empty() || !lib) {
            LogWarn("Spread selection: the selected marker's event has no rule id (%s)", err.c_str());
        } else {
            std::vector<SoundPlacement>             placements;
            std::vector<std::pair<std::string, int>> played;
            for (MediaItem* it : attach_items) {
                ItemFacts f;
                if (!ReadItemFacts(it, &f)) continue;
                int v = -1;
                for (const SoundNode& n : lib->nodes)
                    if (n.type == SoundNodeType::Sound && SameGuid(n.sound.source.item_guid, f.guid)) v = PlayedVariation(n.sound, it);
                played.push_back({f.guid, v});
                SoundPlacement p;
                p.item_guid = f.guid;
                p.own = Ownership::Attached;
                p.offset_s = layer_offset;
                p.track_guid = f.track_guid;
                p.variation = v;
                placements.push_back(p);
            }
            const std::vector<SoundDraw> draws = PlayedDraws(*lib, played);
            const AttachEvent            ev = at_marker;
            ModifyItemSounds(mk.item, nullptr, [&](ItemSounds& rec) {
                for (const SoundPlacement& p : placements) AddToRecord(rec, ev.rid, ev.clip_t, p);
                if (EventSounds* e = FindEventSounds(rec, ev.rid, ev.clip_t))
                    for (const SoundDraw& d : draws) {
                        auto same = std::find_if(e->draws.begin(), e->draws.end(), [&d](const SoundDraw& x) { return x.node == d.node; });
                        if (same != e->draws.end()) *same = d;
                        else e->draws.push_back(d);
                    }
                return true;
            });
            const std::string owner = ItemGuidOf(mk.item);
            for (MediaItem* it : attach_items) WritePlacedTag(it, AttachTagOf(owner, ev));
        }
    }

    PopulateCounts    pc;
    int               events = 0;
    const SoundReport rep = PopulateItemsCounted(targets, &events, &pc);
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
        return Run(scope, false);
    } catch (...) {
        return Finish("Spread selection failed");
    }
}

std::string AddSelectionToStack(SpreadScope scope)
{
    try {
        return Run(scope, true);
    } catch (...) {
        return Finish("Add to stack failed");
    }
}

}  // namespace rav

#else  // host builds

namespace rav {
std::string SpreadSelectionToMarkerType(SpreadScope) { return {}; }
std::string AddSelectionToStack(SpreadScope) { return {}; }
}  // namespace rav

#endif
