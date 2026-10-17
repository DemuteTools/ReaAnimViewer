// SPDX-License-Identifier: MIT
//
// See sound_spread_selection.h (13-68).

#include "sound_spread_selection.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <utility>
#include <vector>

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
#include "sound_place.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "sound_record.h"
#include "sound_tracks.h"
#include "tag_markers.h"
#include "tagging_session.h"

namespace rav {
namespace {

using SetMediaItemInfo_ValueFn = bool (*)(MediaItem* item, const char* parmname, double newvalue);
using MoveMediaItemToTrackFn = bool (*)(MediaItem* item, MediaTrack* desttr);
using GetParentTrackFn = MediaTrack* (*)(MediaTrack* track);

SetMediaItemInfo_ValueFn f_set_item = nullptr;
MoveMediaItemToTrackFn   f_move_to_track = nullptr;
GetParentTrackFn         f_parent_track = nullptr;

constexpr const char kNoMarker[] = "Select a RAV marker (click its label)";
constexpr const char kTakeOnly[] = "RAV markers are take markers only: set Markers to Project or Both";

std::string TrackGuidOf(MediaTrack* tr)
{
    char buf[64] = {};
    if (!tr || !GetSetMediaTrackInfo_String(tr, "GUID", buf, false)) return {};
    return buf;
}

// A GUID compared without case (SameGuid).
std::string GuidKey(std::string g)
{
    for (char& c : g) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return g;
}

uint64_t NewSeed()
{
    const std::string s = NewSoundUid();
    const uint64_t    v = std::strtoull(s.c_str(), nullptr, 16);
    return v ? v : 1;
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

// One animation item of the run.
struct ScopeItem {
    MediaItem*               item = nullptr;
    std::string              guid;
    std::vector<AttachEvent> events;  // its markers of the type (block set, rid filled in the undo scope)
    ItemSounds               rec;
    bool                     detached = false;  // a selected item left one of its events
};

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

// P7: the tracks' items as spans, read once per track (the moving piles' items left out), plus what
// the run places.
class Lanes {
public:
    Lanes(ReaProject* proj, std::set<MediaItem*> moving, MediaTrack* parent)
        : proj_(proj), moving_(std::move(moving)), parent_(parent)
    {
    }
    int made() const { return made_; }

    // The track a layer spanning [start, end) lands on, its own track `own` first, at or after
    // track index `*after` (updated). nullptr when no track could be made.
    MediaTrack* Pick(MediaTrack* own, double start, double end, int* after)
    {
        std::vector<LaneOption>  opts;
        std::vector<MediaTrack*> tracks;
        auto add = [&](MediaTrack* t, bool own_track) {
            if (!t) return;
            const Lane& l = Of(t);
            if (!own_track && !l.usable) return;
            opts.push_back({TrackIndex(proj_, t), !SpanOverlaps(l.spans, start, end)});
            tracks.push_back(t);
        };
        add(own, true);
        if (parent_) {
            const int fi = TrackIndex(proj_, parent_);
            if (fi >= 0) {
                int last = fi, beyond = 0;
                for (int k : DirectChildren(proj_, fi, &last, &beyond)) {
                    MediaTrack* t = GetTrack(proj_, k);
                    if (t != own) add(t, false);
                }
            }
        }
        const int   c = ChooseLane(opts, *after);
        MediaTrack* to = c >= 0 ? tracks[static_cast<size_t>(c)] : nullptr;
        if (!to) {
            std::string name = TrackName(own);
            if (name.empty()) name = "Sounds";
            if (parent_) {
                to = AddChildTrack(proj_, parent_, name);
            } else if (CanMakeTracks()) {
                // No parent folder: below the last track holding a layer so far (or the own track).
                const int at = std::max(TrackIndex(proj_, own), *after) + 1;
                to = AddTrackAt(proj_, at, name);
            }
            if (!to) return nullptr;
            ++made_;
            lanes_[to] = Lane{};
        }
        *after = TrackIndex(proj_, to);
        Of(to).spans.push_back({start, end});
        return to;
    }

private:
    struct Lane {
        std::vector<TimeSpan> spans;
        bool                  usable = true;  // not a folder, no animation item on it
    };
    Lane& Of(MediaTrack* t)
    {
        auto f = lanes_.find(t);
        if (f != lanes_.end()) return f->second;
        Lane l;
        l.usable = GetMediaTrackInfo_Value(t, "I_FOLDERDEPTH") < 1.0;
        const int n = CountTrackMediaItems(t);
        for (int i = 0; i < n; ++i) {
            MediaItem* it = GetTrackMediaItem(t, i);
            if (!it || moving_.count(it)) continue;
            if (RavTakeOf(it)) l.usable = false;
            const double p = GetMediaItemInfo_Value(it, "D_POSITION");
            l.spans.push_back({p, p + GetMediaItemInfo_Value(it, "D_LENGTH")});
        }
        return lanes_[t] = l;
    }

    ReaProject*                 proj_;
    std::set<MediaItem*>        moving_;
    MediaTrack*                 parent_;
    std::map<MediaTrack*, Lane> lanes_;
    int                         made_ = 0;
};

// The gesture's batch while copies are placed (one redraw, each source chunk read once).
struct PlaceBatchScope {
    PlaceBatchScope() { PlaceBatch(true); }
    ~PlaceBatchScope() { PlaceBatch(false); }
};

std::string Finish(const std::string& text)
{
    LogInfo("Spread selection: %s", text.c_str());
    ShowReaperToast(text);
    return text;
}

std::string Run(SpreadScope scope, bool add_layer)
{
    if (!f_set_item || !f_move_to_track) return Finish("REAPER functions missing");
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
    // The selection.
    std::vector<SelectedSound> sel;
    int                        skipped = 0;
    std::string                skip;
    ReadSelectedSounds(&sel, &skipped, &skip);
    if (sel.empty()) return Finish("Select the sound items first" + (skip.empty() ? std::string() : " (" + skip + ")"));

    // The items and their markers of the type (read before anything is written).
    std::vector<ScopeItem> items;
    {
        BusyScope busy("Reading markers");
        ScopeItem first;
        first.item = mk.item;
        if (!MarkersOfType(mk.item, type, &first.events) || first.events.empty())
            return Finish("No " + type + " marker on this animation");
        items.push_back(first);
        if (scope == SpreadScope::AllInstances) {
            for (size_t k = 0; k < rav_items.size(); ++k) {
                MediaItem* it = rav_items[k];
                busy.Item(static_cast<int>(k), static_cast<int>(rav_items.size()));
                if (it == mk.item) continue;
                ItemRulesRead rr;
                if (!ReadItemRules(it, &rr) || !rr.valid) continue;
                const std::vector<char> on = BlocksOfType(rr.rules, type);
                if (std::find(on.begin(), on.end(), 1) == on.end()) continue;
                ScopeItem si;
                si.item = it;
                if (MarkersOfType(it, type, &si.events) && !si.events.empty()) items.push_back(si);
            }
        }
    }
    const bool        all = scope == SpreadScope::AllInstances;
    const std::string undo = (add_layer ? "RAV: Add selection to stack on " : "RAV: Spread selection to ") + type
                           + (all ? " (all instances)" : " (this animation)");

    SoundUndoScope scope_undo(proj, undo.c_str());
    BusyScope      busy("Spreading selection");
    // Rule ids (written when missing), records, seeds.
    std::vector<SpreadSelTarget> targets;
    std::vector<uint64_t>        seeds;
    for (size_t i = 0; i < items.size(); ++i) {
        ScopeItem&               si = items[i];
        std::vector<std::string> rids;
        std::string              err;
        if (!ItemRuleIds(si.item, &rids, &err)) {
            if (i == 0) return Finish("Could not spread: " + err);
            si.events.clear();
        }
        si.guid = ItemGuidOf(si.item);
        si.rec = ReadItemSounds(si.item);
        if (si.rec.seed == 0) si.rec.seed = NewSeed();
        // P5b: a new layer draws apart from the stacks already there.
        seeds.push_back(add_layer ? si.rec.seed ^ 0xA5A5A5A55A5A5A5Aull : si.rec.seed);
        for (AttachEvent& e : si.events) {
            e.rid = e.block >= 0 && static_cast<size_t>(e.block) < rids.size() ? rids[static_cast<size_t>(e.block)] : "";
            if (e.rid.empty()) continue;
            SpreadSelTarget t;
            t.item = static_cast<int>(i);
            t.ev = e;
            targets.push_back(t);
        }
    }
    std::vector<AttachItem> facts;
    std::set<std::string>   sel_guids;
    for (const SelectedSound& s : sel) {
        facts.push_back(s.facts);
        sel_guids.insert(GuidKey(s.facts.guid));
    }
    // P5b: the new layer keeps its offset to the stacks: the selection's top item's snap against the
    // nearest marker of the type on the selected marker's item; every marker gets the layer there.
    double shift = 0.0;
    if (add_layer && !facts.empty()) {
        std::vector<int> every;
        for (size_t i = 0; i < facts.size(); ++i) every.push_back(static_cast<int>(i));
        const double anchor = PileAnchor(facts[static_cast<size_t>(TopmostOf(facts, every))]);
        double       best = -1.0;
        for (const SpreadSelTarget& t : targets)
            if (t.item == 0 && (best < 0.0 || std::fabs(anchor - t.ev.project_t) < best)) {
                best = std::fabs(anchor - t.ev.project_t);
                shift = anchor - t.ev.project_t;
            }
        for (SpreadSelTarget& t : targets) t.ev.project_t += shift;
    }
    const SpreadSelPlan plan = PlanSpreadSelection(facts, targets, seeds);

    SpreadSelCounts counts;
    counts.marker = type;
    counts.all_instances = all;
    counts.selected_markers = n_selected;
    counts.markers = static_cast<int>(targets.size());
    counts.piles = static_cast<int>(plan.piles.size());
    counts.left_over = plan.left_over;
    counts.skip_note = skip;
    {
        std::set<int> with;
        for (const SpreadSelTarget& t : targets) with.insert(t.item);
        counts.items = static_cast<int>(with.size());
    }

    // P5: the attached items and spread copies already on the target markers go first (Populate's
    // stay; the selection's own items are re-attached below).
    auto target_of = [&](const PlacedTag& tag) {
        for (const SpreadSelTarget& t : targets)
            if (SameGuid(tag.owner, items[static_cast<size_t>(t.item)].guid) && tag.rid == t.ev.rid
                && std::fabs(tag.t - t.ev.clip_t) <= kEventMatchSeconds)
                return t.item;
        return -1;
    };
    if (!add_layer) {  // P5b: Add to stack keeps what is there
        std::vector<std::pair<MediaItem*, int>> drop;
        const int                               n = CountMediaItems(proj);
        for (int i = 0; i < n; ++i) {
            MediaItem* it = GetMediaItem(proj, i);
            PlacedTag  tag;
            if (!it || !ReadPlacedTag(it, &tag)) continue;
            if (tag.own != Ownership::Attached && tag.own != Ownership::Spread) continue;
            const int owner = target_of(tag);
            if (owner < 0 || sel_guids.count(GuidKey(ItemGuidOf(it)))) continue;
            drop.push_back({it, owner});
        }
        for (const auto& d : drop) {
            const std::string guid = ItemGuidOf(d.first);
            if (RemovePlacedAny(proj, d.first)) {
                DetachFromRecord(items[static_cast<size_t>(d.second)].rec, guid);
                ++counts.replaced;
            }
        }
        // Record lines of items already gone (attached / spread, not the selection's) on the targets.
        std::set<std::string> present;
        for (int i = 0, m = CountMediaItems(proj); i < m; ++i)
            if (MediaItem* it = GetMediaItem(proj, i)) present.insert(GuidKey(ItemGuidOf(it)));
        for (const SpreadSelTarget& t : targets)
            if (EventSounds* ev = FindEventSounds(items[static_cast<size_t>(t.item)].rec, t.ev.rid, t.ev.clip_t))
                ev->placed.erase(std::remove_if(ev->placed.begin(), ev->placed.end(),
                                                [&](const SoundPlacement& p) {
                                                    return (p.own == Ownership::Attached || p.own == Ownership::Spread)
                                                        && !sel_guids.count(GuidKey(p.item_guid))
                                                        && !present.count(GuidKey(p.item_guid));
                                                }),
                                 ev->placed.end());
    }

    // P7: the lanes. The parent folder of the selected assets: the top item's track's parent.
    std::vector<MediaTrack*> src_track(sel.size(), nullptr);
    for (size_t i = 0; i < sel.size(); ++i) src_track[i] = GetMediaItem_Track(sel[i].item);
    MediaTrack* parent = nullptr;
    {
        std::vector<int> every;
        for (size_t i = 0; i < facts.size(); ++i) every.push_back(static_cast<int>(i));
        const int top = TopmostOf(facts, every);
        if (top >= 0 && f_parent_track && src_track[static_cast<size_t>(top)])
            parent = f_parent_track(src_track[static_cast<size_t>(top)]);
    }
    std::set<MediaItem*> moving;
    for (const SpreadSelPile& p : plan.piles)
        if (p.target >= 0 && !p.in_place)
            for (int m : p.members) moving.insert(sel[static_cast<size_t>(m)].item);
    Lanes lanes(proj, moving, parent);

    // P6 / P6b: the piles moved onto their markers (originals) and attached.
    ScopeItem& first = items[0];
    for (const SpreadSelPile& p : plan.piles) {
        if (p.target < 0) continue;
        const AttachEvent& ev = targets[static_cast<size_t>(p.target)].ev;
        int                after = -1;
        for (int m : p.members) {
            SelectedSound& s = sel[static_cast<size_t>(m)];
            if (!p.in_place) {
                const double pos = MemberLanding(ev.project_t, p.anchor, s.facts.pos);
                MediaTrack*  own = src_track[static_cast<size_t>(m)];
                MediaTrack*  to = lanes.Pick(own, pos, pos + s.facts.len, &after);
                if (!to) ++counts.failed;  // no track could be made: moved on its own track
                else if (to != own) {
                    if (f_move_to_track(s.item, to)) s.facts.track_guid = TrackGuidOf(to);
                    else ++counts.failed;
                }
                f_set_item(s.item, "D_POSITION", pos);
                RefreshLaneItem(s.item);
            }
            // D3: an attached item leaves its old event.
            if (s.tagged && s.tag.own == Ownership::Attached) {
                bool in_run = false;
                for (ScopeItem& si : items)
                    if (SameGuid(s.tag.owner, si.guid)) {
                        si.detached = DetachFromRecord(si.rec, s.facts.guid) || si.detached;
                        in_run = true;
                    }
                if (!in_run)
                    if (MediaItem* owner = ScanItemByGuid(proj, s.tag.owner)) {
                        const std::string g = s.facts.guid;
                        ModifyItemSounds(owner, nullptr, [&g](ItemSounds& r) { return DetachFromRecord(r, g); });
                    }
            }
            const double offset = (p.in_place ? p.anchor - ev.project_t : 0.0) + shift;
            AddToRecord(first.rec, ev.rid, ev.clip_t, AttachPlacementOf(s.facts, offset));
            WritePlacedTag(s.item, AttachTagOf(first.guid, ev));
        }
        ++counts.attached;
    }

    // P4 / P7: the spread copies, one pile per other marker.
    {
        PlaceBatchScope batch;
        for (size_t k = 0; k < plan.copies.size(); ++k) {
            BusyStep(static_cast<int>(k), static_cast<int>(plan.copies.size()), "marker");
            const SpreadSelCopy&   c = plan.copies[k];
            const SpreadSelTarget& t = targets[static_cast<size_t>(c.target)];
            const SpreadSelPile&   p = plan.piles[static_cast<size_t>(c.pile)];
            ScopeItem&             si = items[static_cast<size_t>(t.item)];
            int                    after = -1;
            bool                   any = false, layer_failed = false;
            for (int m : p.members) {
                const SelectedSound& s = sel[static_cast<size_t>(m)];
                const double         pos = MemberLanding(t.ev.project_t, p.anchor, s.facts.pos);
                MediaTrack*          to = lanes.Pick(src_track[static_cast<size_t>(m)], pos, pos + s.facts.len, &after);
                if (!to) {
                    layer_failed = true;
                    continue;
                }
                PlaceRequest req;
                req.owner = si.item;
                req.rid = t.ev.rid;
                req.event_t = t.ev.clip_t;
                req.project_t = t.ev.project_t;
                req.own = Ownership::Spread;
                req.source.kind = SoundSourceKind::Item;
                req.source.item_guid = s.facts.guid;
                req.variation = (c.take >= 0 && s.facts.takes > c.take) ? c.take : -1;  // P4 on takes
                req.offset_s = SpreadMemberOffset(p.anchor, s.facts.pos, s.facts.snap, 0.0);
                req.track = to;
                const PlaceResult pr = PlaceSounds(proj, req);
                if (!pr.ok) {
                    layer_failed = true;
                    LogWarn("Spread selection: could not place a copy: %s", pr.error.c_str());
                    continue;
                }
                const std::string tg = TrackGuidOf(to);
                for (const std::string& g : pr.item_guids) {
                    SoundPlacement sp;
                    sp.item_guid = g;
                    sp.own = Ownership::Spread;
                    sp.variation = req.variation;
                    sp.track_guid = tg;
                    AddToRecord(si.rec, t.ev.rid, t.ev.clip_t, sp);
                }
                any = true;
            }
            if (any) ++counts.copies;
            if (layer_failed) ++counts.failed;  // once per copy
        }
    }
    counts.tracks = lanes.made();
    std::set<int> with_targets;
    for (const SpreadSelTarget& t : targets) with_targets.insert(t.item);
    for (size_t i = 0; i < items.size(); ++i) {
        ScopeItem& si = items[i];
        if (!with_targets.count(static_cast<int>(i)) && !si.detached) continue;  // an item the run did not touch
        const ItemSounds rec = si.rec;
        ModifyItemSounds(si.item, nullptr, [&rec](ItemSounds& r) {
            r = rec;
            return true;
        });
    }
    UpdateArrange();
    return Finish((add_layer ? std::string("Added to stack: ") : std::string()) + SpreadSelectionReport(counts));
}

}  // namespace

void InitSoundSpreadSelection(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_set_item = reinterpret_cast<SetMediaItemInfo_ValueFn>(get_func("SetMediaItemInfo_Value"));
    f_move_to_track = reinterpret_cast<MoveMediaItemToTrackFn>(get_func("MoveMediaItemToTrack"));
    f_parent_track = reinterpret_cast<GetParentTrackFn>(get_func("GetParentTrack"));
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
