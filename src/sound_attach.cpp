// SPDX-License-Identifier: MIT
//
// See sound_attach.h (13-10).

#include "sound_attach.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "item_rules.h"
#include "rule_record.h"
#include "sound_draw.h"
#include "sound_entries.h"
#include "sound_place.h"
#include "sound_place_plan.h"
#include "sound_project.h"
#include "sound_record.h"
#include "tagging_session.h"

namespace rav {
namespace {

using AddMediaItemToTrackFn = MediaItem* (*)(MediaTrack* tr);
using GetItemStateChunkFn = bool (*)(MediaItem* item, char* strNeedBig, int strNeedBig_sz, bool isundo);
using SetItemStateChunkFn = bool (*)(MediaItem* item, const char* str, bool isundo);
using SetMediaItemInfo_ValueFn = bool (*)(MediaItem* item, const char* parmname, double newvalue);
using GetMediaItemTakeFn = MediaItem_Take* (*)(MediaItem* item, int tk);
using CountTakesFn = int (*)(MediaItem* item);

AddMediaItemToTrackFn    f_add_item = nullptr;
GetItemStateChunkFn      f_get_chunk = nullptr;
SetItemStateChunkFn      f_set_chunk = nullptr;
SetMediaItemInfo_ValueFn f_set_item = nullptr;
GetMediaItemTakeFn       f_get_take = nullptr;
CountTakesFn             f_count_takes = nullptr;

constexpr const char kMissing[] = "REAPER functions missing";

bool Ready()
{
    return f_add_item && f_get_chunk && f_set_chunk && f_set_item && f_get_take && f_count_takes;
}

ReaProject* CurProject() { return EnumProjects(-1, nullptr, 0); }

std::string ItemGuidOf(MediaItem* item)
{
    char buf[64] = {};
    if (!item || !GetSetMediaItemInfo_String(item, "GUID", buf, false)) return {};
    return buf;
}

bool SameGuid(const std::string& a, const std::string& b)
{
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
        if (x != y) return false;
    }
    return true;
}

MediaItem* FindItemByGuid(ReaProject* proj, const std::string& guid)
{
    if (guid.empty()) return nullptr;
    const int n = CountMediaItems(proj);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (it && SameGuid(ItemGuidOf(it), guid)) return it;
    }
    return nullptr;
}

std::string NewGuidString()
{
    GUID g;
    genGuid(&g);
    char buf[64] = {};
    guidToString(&g, buf);
    return buf;
}

std::string ReadChunk(MediaItem* item)
{
    if (!f_get_chunk || !item) return {};
    for (int size = 64 * 1024; size <= 64 * 1024 * 1024; size *= 2) {
        std::vector<char> buf(static_cast<size_t>(size), '\0');
        if (!f_get_chunk(item, buf.data(), size, false)) return {};
        const size_t len = std::strlen(buf.data());
        if (len + 1 < static_cast<size_t>(size)) return std::string(buf.data(), len);
    }
    return {};
}

// One selected item.
struct SelItem {
    MediaItem* item = nullptr;
    AttachItem facts;
    bool       tagged = false;  // carries RAV_SND (eligible: attached)
    PlacedTag  tag;
};

// The selected items: the eligible ones, and how many were skipped and why.
void ReadSelection(std::vector<SelItem>* eligible, int* skipped, std::string* note)
{
    *skipped = 0;
    std::vector<std::string> reasons;
    const int                n = CountSelectedMediaItems(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetSelectedMediaItem(nullptr, i);
        if (!it) continue;
        SelItem s;
        s.item = it;
        s.tagged = ReadPlacedTag(it, &s.tag);
        const bool        rav = RavTakeOf(it) != nullptr;
        const bool        folder = !rav && IsFolderItem(it);
        const std::string why = AttachSkipReason(rav, folder, s.tagged, s.tag.own);
        if (!why.empty()) {
            ++*skipped;
            if (std::find(reasons.begin(), reasons.end(), why) == reasons.end()) reasons.push_back(why);
            continue;
        }
        ItemFacts f;
        if (!ReadItemFacts(it, &f)) {
            ++*skipped;
            if (std::find(reasons.begin(), reasons.end(), "unreadable item") == reasons.end())
                reasons.push_back("unreadable item");
            continue;
        }
        s.facts.guid = f.guid;
        s.facts.track_guid = f.track_guid;
        MediaTrack* tr = GetMediaItem_Track(it);
        s.facts.track = tr ? static_cast<int>(GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER")) : 0;
        s.facts.pos = f.position;
        s.facts.len = f.length;
        s.facts.snap = f.snap_offset;
        s.facts.takes = f.takes;
        eligible->push_back(s);
    }
    note->clear();
    if (*skipped > 0) {
        *note = std::to_string(*skipped) + " skipped";
        for (size_t i = 0; i < reasons.size(); ++i) *note += (i == 0 ? ": " : " / ") + reasons[i];
    }
}

std::vector<AttachItem> FactsOf(const std::vector<SelItem>& sel)
{
    std::vector<AttachItem> out;
    for (const SelItem& s : sel) out.push_back(s.facts);
    return out;
}

std::string Plural(int n, const char* word)
{
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

std::string WithSkipped(std::string note, const std::string& skip)
{
    if (!skip.empty()) note += note.empty() ? skip : ", " + skip;
    return note;
}

// The rid of every block of the animation item, giving the rules ids first (pool-aware, inside
// the caller's undo scope). False when the rules cannot be read or written.
bool RuleIds(MediaItem* anim, std::vector<std::string>* rids, std::string* err)
{
    ItemRulesRead rr;
    if (!ReadItemRules(anim, &rr)) {
        *err = "not an animation item";
        return false;
    }
    bool missing = !rr.valid;
    for (const Block& b : rr.rules.blocks) missing = missing || b.rid.empty();
    if (missing) {
        if (!ModifyItemRules(anim, nullptr, [](ItemRules& r) { return EnsureRuleIds(&r); }, err)) return false;
        rr = ItemRulesRead{};
        if (!ReadItemRules(anim, &rr) || !rr.valid) {
            *err = "the rules do not read";
            return false;
        }
    }
    rids->clear();
    for (const Block& b : rr.rules.blocks) rids->push_back(b.rid);
    return true;
}

std::string RidOf(const std::vector<std::string>& rids, int block)
{
    return block >= 0 && static_cast<size_t>(block) < rids.size() ? rids[static_cast<size_t>(block)] : std::string();
}

// D3: an attached item leaves its old event (in its old owner's record; `rec` when the owner is
// the animation item itself).
void LeaveOldEvent(ReaProject* proj, const SelItem& s, MediaItem* anim, const std::string& anim_guid, ItemSounds& rec)
{
    if (!s.tagged) return;
    if (SameGuid(s.tag.owner, anim_guid)) {
        DetachFromRecord(rec, s.facts.guid);
        return;
    }
    MediaItem* owner = FindItemByGuid(proj, s.tag.owner);
    if (!owner || owner == anim) return;
    const std::string guid = s.facts.guid;
    ModifyItemSounds(owner, nullptr, [&guid](ItemSounds& r) { return DetachFromRecord(r, guid); });
}

// Attaches the pile `members` (indices into sel) to `ev`.
void AttachPile(ReaProject* proj, MediaItem* anim, const std::string& anim_guid, const std::vector<SelItem>& sel,
                const AttachPilePlan& pile, const AttachEvent& ev, ItemSounds& rec)
{
    for (int i : pile.members) {
        const SelItem& s = sel[static_cast<size_t>(i)];
        LeaveOldEvent(proj, s, anim, anim_guid, rec);
        AddToRecord(rec, ev.rid, ev.clip_t, AttachPlacementOf(s.facts, pile.offset_s));
        WritePlacedTag(s.item, AttachTagOf(anim_guid, ev));
    }
}

bool WriteRecord(MediaItem* anim, const ItemSounds& rec)
{
    return ModifyItemSounds(anim, nullptr, [&rec](ItemSounds& r) {
        r = rec;
        return true;
    });
}

// The shared body of the two attach gestures. `block` -1 = nearest of any rule, one pile.
AttachResult AttachImpl(MediaItem* anim, const std::vector<AttachEvent>& events_in, int block, const char* undo)
{
    AttachResult        res;
    std::vector<SelItem> sel;
    int                 skipped = 0;
    std::string         skip;
    ReadSelection(&sel, &skipped, &skip);
    if (!anim) {
        res.note = "No animation item";
        return res;
    }
    if (sel.empty()) {
        res.note = WithSkipped("Nothing to attach", skip);
        return res;
    }
    // Plan first: nothing is written when no pile has an event.
    const std::vector<AttachPilePlan> piles = PlanAttach(FactsOf(sel), events_in, block);
    int                               found = 0;
    for (const AttachPilePlan& p : piles)
        if (p.event >= 0) ++found;
    if (found == 0) {
        res.note = WithSkipped(block < 0 ? "No event to attach to" : "No event of this rule", skip);
        return res;
    }
    ReaProject*    proj = CurProject();
    SoundUndoScope scope(proj, undo);
    std::vector<std::string> rids;
    std::string              err;
    if (!RuleIds(anim, &rids, &err)) {
        res.note = "Could not attach: " + err;
        return res;
    }
    const std::string anim_guid = ItemGuidOf(anim);
    ItemSounds        rec = ReadItemSounds(anim);
    int               n_items = 0, n_groups = 0, n_none = 0;
    for (const AttachPilePlan& pile : piles) {
        AttachEvent ev;
        if (pile.event >= 0) {
            ev = events_in[static_cast<size_t>(pile.event)];
            ev.rid = RidOf(rids, ev.block);
        }
        if (ev.rid.empty()) {
            n_none += static_cast<int>(pile.members.size());
            continue;
        }
        AttachPile(proj, anim, anim_guid, sel, pile, ev, rec);
        n_items += static_cast<int>(pile.members.size());
        ++n_groups;
    }
    WriteRecord(anim, rec);
    UpdateArrange();
    res.wrote = n_items > 0;
    res.note = "Attached " + Plural(n_items, "item");
    if (block >= 0 && n_groups > 1) res.note += " (" + Plural(n_groups, "group") + ")";
    if (n_none > 0) res.note += ", " + std::to_string(n_none) + " with no event of this rule";
    res.note = WithSkipped(res.note, skip);
    return res;
}

uint64_t NewSeed()
{
    const std::string s = NewSoundUid();
    uint64_t          v = std::strtoull(s.c_str(), nullptr, 16);
    return v ? v : 1;
}

// The take facts of an item, for a promoted Sound (D7: from takes only).
SoundData PromotedSoundData(MediaItem* item)
{
    SoundData d;
    d.source.kind = SoundSourceKind::Item;
    d.source.item_guid = ItemGuidOf(item);
    EntryFacts      f;
    MediaItem_Take* take = GetActiveTake(item);
    f.item_len = GetMediaItemInfo_Value(item, "D_LENGTH");
    if (take) {
        f.start_offs = GetMediaItemTakeInfo_Value(take, "D_STARTOFFS");
        PCM_source* src = GetMediaItemTake_Source(take);
        bool        qn = false;
        f.source_len = src ? GetMediaSourceLength(src, &qn) : 0.0;
    }
    const int n = f_count_takes(item);
    if (n >= 2) {
        for (int k = 0; k < n; ++k) {
            MediaItem_Take* t = f_get_take(item, k);
            SoundFrame      fr;
            fr.start = t ? GetMediaItemTakeInfo_Value(t, "D_STARTOFFS") : 0.0;
            fr.length = 0.0;  // the item's length (DetectFrames)
            f.takes.push_back(fr);
        }
    }
    d.mode = DefaultDetectMode(f, false);
    d.frames = DetectFrames(f, d.mode, d.silence, nullptr);
    return d;
}

// E2: the clip time of the event of rule `block` the pile's top item is attached to (on this
// animation item); false when it is not attached there. Reads the rules without writing: a
// block with no rid has no attached item yet.
bool AttachedClipT(MediaItem* anim, const AttachItem& top, const std::vector<SelItem>& sel, int block, double* t)
{
    ItemRulesRead rr;
    if (!anim || block < 0 || !ReadItemRules(anim, &rr) || static_cast<size_t>(block) >= rr.rules.blocks.size()) return false;
    const std::string& rid = rr.rules.blocks[static_cast<size_t>(block)].rid;
    const std::string  anim_guid = ItemGuidOf(anim);
    for (const SelItem& s : sel)
        if (s.facts.guid == top.guid && s.tagged && s.tag.own == Ownership::Attached && !rid.empty() && s.tag.rid == rid
            && SameGuid(s.tag.owner, anim_guid)) {
            *t = s.tag.t;
            return true;
        }
    return false;
}

std::string TakeNameOf(MediaItem* item)
{
    MediaItem_Take* take = GetActiveTake(item);
    const char*     n = take ? GetTakeName(take) : nullptr;
    return n && *n ? std::string(n) : std::string("Sound");
}

}  // namespace

void InitSoundAttach(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_add_item = reinterpret_cast<AddMediaItemToTrackFn>(get_func("AddMediaItemToTrack"));
    f_get_chunk = reinterpret_cast<GetItemStateChunkFn>(get_func("GetItemStateChunk"));
    f_set_chunk = reinterpret_cast<SetItemStateChunkFn>(get_func("SetItemStateChunk"));
    f_set_item = reinterpret_cast<SetMediaItemInfo_ValueFn>(get_func("SetMediaItemInfo_Value"));
    f_get_take = reinterpret_cast<GetMediaItemTakeFn>(get_func("GetMediaItemTake"));
    f_count_takes = reinterpret_cast<CountTakesFn>(get_func("CountTakes"));
}

std::vector<AttachEvent> AttachEventsOf(const std::vector<ShownEvent>& shown, const ItemClipMap& map)
{
    std::vector<AttachEvent> out;
    for (const ShownEvent& e : shown) {
        if (e.kind != ShownKind::Detected && e.kind != ShownKind::User) continue;
        for (double p : ClipToProjectTimes(map, e.t)) {
            AttachEvent a;
            a.block = e.block;
            a.clip_t = e.t;
            a.project_t = p;
            out.push_back(a);
        }
    }
    return out;
}

SoundReport AttachSelectedTo(MediaItem* anim_item)
{
    SoundReport rep;
    if (!anim_item) return rep;
    if (!Ready()) {
        rep.lines.push_back({ReportKind::Blocked, kMissing, {}, {}, 0.0});
        return rep;
    }
    const ItemDetection det = DetectItem(anim_item);
    if (det.status != ItemDetection::Status::Ok) {
        rep.lines.push_back({ReportKind::Blocked, "the animation item has no events", {}, {}, 0.0});
        return rep;
    }
    const std::vector<ShownEvent> shown = BuildEventList(det.events, det.rules.events, det.rules.blocks.size());
    AttachResult r = AttachToNearest(anim_item, AttachEventsOf(shown, det.map));
    rep = r.report;
    if (!r.wrote) rep.lines.push_back({ReportKind::Blocked, r.note, {}, {}, 0.0});
    return rep;
}

AttachSelectionInfo ReadAttachSelection()
{
    AttachSelectionInfo  info;
    std::vector<SelItem> sel;
    ReadSelection(&sel, &info.skipped, &info.skip_note);
    info.eligible = static_cast<int>(sel.size());
    for (const SelItem& s : sel)
        if (s.tagged && s.tag.own == Ownership::Attached) ++info.attached;
    return info;
}

AttachResult AttachToNearest(MediaItem* anim_item, const std::vector<AttachEvent>& events)
{
    if (!Ready()) return {false, kMissing, {}};
    return AttachImpl(anim_item, events, -1, "RAV: Attach to nearest event");
}

AttachResult AttachToMarkers(MediaItem* anim_item, const std::vector<AttachEvent>& events, int block)
{
    if (!Ready()) return {false, kMissing, {}};
    if (block < 0) return {false, "No rule", {}};
    return AttachImpl(anim_item, events, block, "RAV: Attach to markers");
}

AttachResult SpreadTo(MediaItem* anim_item, const std::vector<AttachEvent>& events_in, int block)
{
    AttachResult res;
    if (!Ready()) {
        res.note = kMissing;
        return res;
    }
    std::vector<SelItem> sel;
    int                  skipped = 0;
    std::string          skip;
    ReadSelection(&sel, &skipped, &skip);
    if (!anim_item || sel.empty()) {
        res.note = WithSkipped("Nothing to spread", skip);
        return res;
    }
    const std::vector<AttachItem> facts = FactsOf(sel);
    std::vector<int>              all;
    for (size_t i = 0; i < sel.size(); ++i) all.push_back(static_cast<int>(i));
    const AttachItem& top = facts[static_cast<size_t>(TopmostOf(facts, all))];
    const double      anchor = PileAnchor(top);
    // D5: the rule's events but the one the pile sits on (also when the top item is attached to
    // an event of this rule away from its anchor), in time order.
    double                         attached_t = 0.0;
    const bool                     attached = AttachedClipT(anim_item, top, sel, block, &attached_t);
    const std::vector<AttachEvent> targets = SpreadTargets(events_in, block, anchor, attached ? &attached_t : nullptr);
    if (targets.empty()) {
        res.note = WithSkipped("No other event of this rule", skip);
        return res;
    }
    ReaProject*              proj = CurProject();
    SoundUndoScope           scope(proj, "RAV: Spread sound");
    std::vector<std::string> rids;
    std::string              err;
    if (!RuleIds(anim_item, &rids, &err)) {
        res.note = "Could not spread: " + err;
        return res;
    }
    const std::string rid = RidOf(rids, block);
    if (rid.empty()) {
        res.note = "Could not spread: the rule has no id";
        return res;
    }
    ItemSounds rec = ReadItemSounds(anim_item);
    if (rec.seed == 0) rec.seed = NewSeed();

    // D5: every target event's previous spread copies are removed first (several project times
    // may share one event), their nudge kept.
    std::vector<double>          nudge_of;  // per target
    const std::vector<TakenCopy> taken = TakeSpreadCopies(rec, rid, targets, &nudge_of);
    const int                    replaced = static_cast<int>(taken.size());
    for (const TakenCopy& c : taken) {
        RemovePlacement(proj, c.p.item_guid);
        res.report.lines.push_back({ReportKind::Replaced, "spread copy replaced", c.p.node, c.p.item_guid, c.t});
    }

    // D6: the take pick per event (the passes of one event share it).
    const std::vector<int> picks = SpreadPicks(targets, SpreadPickTakes(facts), rec.seed, rid);
    int                    n_events = 0, n_copies = 0, n_failed = 0;
    for (size_t k = 0; k < targets.size(); ++k) {
        const AttachEvent& ev = targets[k];
        const int          pick = picks[k];
        bool               any = false;
        for (const AttachItem& m : facts) {
            PlaceRequest req;
            req.owner = anim_item;
            req.rid = rid;
            req.event_t = ev.clip_t;
            req.project_t = ev.project_t;
            req.own = Ownership::Spread;
            req.source.kind = SoundSourceKind::Item;
            req.source.item_guid = m.guid;
            req.variation = (pick >= 0 && m.takes > pick) ? pick : -1;
            req.offset_s = SpreadMemberOffset(anchor, m.pos, m.snap, nudge_of[k]);
            const PlaceResult pr = PlaceSounds(proj, req);
            if (!pr.ok) {
                ++n_failed;
                res.report.lines.push_back({ReportKind::Blocked, "could not spread: " + pr.error, {}, m.guid, ev.clip_t});
                continue;
            }
            for (const std::string& g : pr.item_guids) {
                SoundPlacement p;
                p.item_guid = g;
                p.own = Ownership::Spread;
                p.offset_s = nudge_of[k];
                p.variation = req.variation;
                p.track_guid = m.track_guid;
                AddToRecord(rec, rid, ev.clip_t, p);
                ++n_copies;
            }
            any = true;
        }
        if (any) ++n_events;
    }
    WriteRecord(anim_item, rec);
    UpdateArrange();
    res.wrote = n_copies > 0 || replaced > 0;
    res.note = "Spread to " + Plural(n_events, "event") + " (" + std::to_string(n_copies)
             + (n_copies == 1 ? " copy)" : " copies)");
    if (replaced > 0) res.note += ", " + std::to_string(replaced) + " replaced";
    if (n_failed > 0) res.note += ", " + std::to_string(n_failed) + " failed";
    res.note = WithSkipped(res.note, skip);
    return res;
}

AttachResult PromoteToLibrary(const std::string& library_uid, const std::string& value_uid)
{
    AttachResult res;
    if (!Ready()) {
        res.note = kMissing;
        return res;
    }
    std::vector<SelItem> sel;
    int                  skipped = 0;
    std::string          skip;
    ReadSelection(&sel, &skipped, &skip);
    // D7: the topmost selected attached item's event.
    std::vector<AttachItem> facts;
    std::vector<int>        idx;
    for (size_t i = 0; i < sel.size(); ++i)
        if (sel[i].tagged && sel[i].tag.own == Ownership::Attached) {
            idx.push_back(static_cast<int>(facts.size()));
            facts.push_back(sel[i].facts);
        }
    if (idx.empty()) {
        res.note = "Select an attached sound to promote";
        return res;
    }
    const int top_i = TopmostOf(facts, idx);
    PlacedTag key;
    for (const SelItem& s : sel)
        if (s.facts.guid == facts[static_cast<size_t>(top_i)].guid) key = s.tag;
    ReaProject* proj = CurProject();
    const SoundStore& store_now = GetSoundStore(proj);
    const SoundLibrary* lib_now = FindLibrary(store_now, library_uid);
    if (!lib_now) {
        res.note = "The library is gone";
        return res;
    }
    const std::string lib_name = lib_now->name;

    // Every item attached to that event, and the project end.
    std::vector<MediaItem*> members;
    double                  project_end = 0.0;
    const int               n = CountMediaItems(proj);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (!it) continue;
        project_end = std::max(project_end, GetMediaItemInfo_Value(it, "D_POSITION") + GetMediaItemInfo_Value(it, "D_LENGTH"));
        PlacedTag t;
        if (ReadPlacedTag(it, &t) && t.own == Ownership::Attached && SameGuid(t.owner, key.owner) && t.rid == key.rid
            && std::fabs(t.t - key.t) <= kEventMatchSeconds)
            members.push_back(it);
    }
    if (members.empty()) {
        res.note = "Nothing attached to promote";
        return res;
    }
    // Topmost first (lowest track, then earliest).
    auto track_no = [](MediaItem* it) {
        MediaTrack* tr = GetMediaItem_Track(it);
        return tr ? GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER") : 0.0;
    };
    std::stable_sort(members.begin(), members.end(), [&](MediaItem* a, MediaItem* b) {
        const double ta = track_no(a), tb = track_no(b);
        if (ta != tb) return ta < tb;
        return GetMediaItemInfo_Value(a, "D_POSITION") < GetMediaItemInfo_Value(b, "D_POSITION");
    });
    double first_pos = GetMediaItemInfo_Value(members[0], "D_POSITION");
    for (MediaItem* m : members) first_pos = std::min(first_pos, GetMediaItemInfo_Value(m, "D_POSITION"));

    SoundUndoScope          scope(proj, "RAV: Promote to library");
    std::vector<MediaItem*> copies;
    for (MediaItem* m : members) {
        MediaTrack*       tr = GetMediaItem_Track(m);
        const std::string chunk = ReadChunk(m);
        MediaItem*        c = (tr && !chunk.empty()) ? f_add_item(tr) : nullptr;
        if (!c) {
            res.report.lines.push_back({ReportKind::Blocked, "could not copy an item", {}, ItemGuidOf(m), key.t});
            continue;
        }
        const std::string guid = NewGuidString();
        f_set_chunk(c, PrepareCopyChunk(chunk, guid, &NewGuidString).c_str(), false);
        f_set_item(c, "D_POSITION", PromoteCopyPosition(project_end, first_pos, GetMediaItemInfo_Value(m, "D_POSITION")));
        ClearPlacedTag(c);
        copies.push_back(c);
    }
    if (copies.empty()) {
        res.note = "Could not copy the sounds";
        return res;
    }
    // The nodes: one Sound, or a Blend of one Sound per copy.
    std::vector<SoundNode> nodes;
    auto sync_of = [](MediaItem* it) {
        return GetMediaItemInfo_Value(it, "D_POSITION") + GetMediaItemInfo_Value(it, "D_SNAPOFFSET");
    };
    if (copies.size() == 1) {
        SoundNode s;
        s.uid = NewSoundUid();
        s.type = SoundNodeType::Sound;
        s.name = TakeNameOf(copies[0]);
        s.sound = PromotedSoundData(copies[0]);
        nodes.push_back(s);
    } else {
        SoundNode b;
        b.uid = NewSoundUid();
        b.type = SoundNodeType::Blend;
        b.name = TakeNameOf(copies[0]);
        nodes.push_back(b);
        const double top_sync = sync_of(copies[0]);
        for (MediaItem* c : copies) {
            SoundNode s;
            s.uid = NewSoundUid();
            s.parent = b.uid;
            s.type = SoundNodeType::Sound;
            s.name = TakeNameOf(c);
            s.offset_ms = BlendChildOffsetMs(sync_of(c), top_sync);
            s.sound = PromotedSoundData(c);
            nodes.push_back(s);
        }
    }
    std::string err;
    const bool  added = ModifySoundStore(proj, nullptr, [&](SoundStore& st) {
        SoundLibrary* lib = FindLibrary(st, library_uid);
        if (!lib) {
            err = "the library is gone";
            return false;
        }
        std::string value_name;
        const SoundNode* root = FindNode(*lib, lib->root);
        if (root && root->type == SoundNodeType::Switch)
            if (const SwitchGroup* g = FindGroup(st, root->group_uid))
                for (const SwitchValue& v : g->values)
                    if (v.uid == value_uid) value_name = v.name;
        return AddPromotedToLibrary(*lib, nodes, value_uid, NewSoundUid(), &err, value_name);
    });
    UpdateArrange();
    res.wrote = true;
    if (!added) {
        res.note = "Copied " + Plural(static_cast<int>(copies.size()), "item") + ", not added: " + err;
        return res;
    }
    res.note = "Promoted " + Plural(static_cast<int>(copies.size()), "item") + " to " + lib_name;
    return res;
}

}  // namespace rav

#endif  // _WIN32
