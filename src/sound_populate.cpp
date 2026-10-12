// SPDX-License-Identifier: MIT
//
// See sound_populate.h. Populate = resolve (13-4) -> draw (13-3) -> place (13-6), recorded in the
// item's RAV_SOUNDS; the decisions are sound_populate_plan.h's.

#include "sound_populate.h"

#ifdef _WIN32

#include <cmath>
#include <map>
#include <random>
#include <set>

#include "event_list.h"
#include "item_rules.h"
#include "library_user_ui.h"  // 13-11: File Sounds are imported on first use
#include "ravlib.h"
#include "rule_record.h"
#include "sound_draw.h"
#include "sound_follow_plan.h"
#include "sound_place.h"
#include "sound_place_plan.h"
#include "sound_populate_plan.h"
#include "sound_project.h"
#include "sound_record.h"
#include "sound_switch.h"
#include "tagging_session.h"
#include "tagging_signal.h"

namespace rav {

namespace {

using SetMediaItemSelectedFn = void (*)(MediaItem* item, bool selected);
SetMediaItemSelectedFn f_set_selected = nullptr;

constexpr int kUnselectAllItems = 40289;  // Item: Unselect (clear selection of) all items

bool SameGuidText(const std::string& a, const std::string& b)
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

// The project scan behind FindItemByGuid.
MediaItem* FindPlacedItem(const std::string& guid)
{
    const int n = CountMediaItems(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(nullptr, i);
        if (it && SameGuidText(ItemGuidOf(it), guid)) return it;
    }
    return nullptr;
}

// One gesture's GUID -> item map (built once, kept in step with the items the gesture places and
// removes, dropped when the gesture ends: never a pointer kept across gestures).
std::string GuidKey(const std::string& g)
{
    std::string k = g;
    for (char& c : k)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return k;
}

std::map<std::string, MediaItem*>* g_items = nullptr;

// The engine's finder: the gesture's map only (a miss: the engine scans).
MediaItem* IndexedItem(const std::string& guid)
{
    if (!g_items) return nullptr;
    auto hit = g_items->find(GuidKey(guid));
    return hit == g_items->end() ? nullptr : hit->second;
}

class ItemIndexScope {
public:
    ItemIndexScope()
    {
        if (g_items) return;  // nested: the outer gesture's map
        const int n = CountMediaItems(nullptr);
        for (int i = 0; i < n; ++i)
            if (MediaItem* it = GetMediaItem(nullptr, i)) {
                char buf[64] = {};
                if (GetSetMediaItemInfo_String(it, "GUID", buf, false) && buf[0]) map_[GuidKey(buf)] = it;
            }
        g_items = &map_;
        owner_ = true;
        SetPlaceItemFinder(&IndexedItem);
    }
    ~ItemIndexScope()
    {
        if (!owner_) return;
        SetPlaceItemFinder(nullptr);
        g_items = nullptr;
    }
    ItemIndexScope(const ItemIndexScope&) = delete;
    ItemIndexScope& operator=(const ItemIndexScope&) = delete;

private:
    std::map<std::string, MediaItem*> map_;
    bool                              owner_ = false;
};

// RemovePlacement, the item dropped from the gesture's map.
bool RemoveIndexed(const std::string& g)
{
    if (g_items) g_items->erase(GuidKey(g));
    return RemovePlacement(nullptr, g);
}

uint64_t RandomSeed()
{
    std::random_device rd;
    uint64_t s = (static_cast<uint64_t>(rd()) << 32) ^ rd();
    return s ? s : 0x9E3779B97F4A7C15ull;
}

// What one pass over an item does.
struct PopOpts {
    bool        only_populated = false;  // P8: only the events that already have a live placement
    bool        report_replaced = true;
    std::string rid;                      // "" = every rule
    bool        has_t = false;
    double      t = 0.0;
};

std::string NodeName(const SoundNode* n)
{
    if (!n) return "?";
    return n->name.empty() ? n->uid : n->name;
}

void AddLine(SoundReport& rep, ReportKind kind, const std::string& text, const std::string& node,
             const std::string& item_guid, double t)
{
    ReportLine l;
    l.kind = kind;
    l.text = text;
    l.node = node;
    l.item_guid = item_guid;
    l.t = t;
    rep.lines.push_back(l);
}

// P13: every placed item's tag names the animation item.
void CheckOwners(const std::vector<std::string>& guids, const std::string& owner, SoundReport& rep,
                 const std::string& node, double t)
{
    for (const std::string& g : guids) {
        PlacedTag tag;
        MediaItem* it = FindItemByGuid(g);
        if (!it || !ReadPlacedTag(it, &tag) || !SameGuidText(tag.owner, owner))
            AddLine(rep, ReportKind::Blocked, "tag owner mismatch", node, owner, t);
    }
}

// The request for one resolved sound (P11).
PlaceRequest RequestFor(MediaItem* owner, const std::string& rid, double event_t, double project_t,
                        const SoundNode& n, const ResolvedSound& rs,
                        const std::map<std::string, std::string>* materialised = nullptr)
{
    PlaceRequest req;
    req.owner = owner;
    req.rid = rid;
    req.event_t = event_t;
    req.project_t = project_t;
    req.node = n.uid;
    req.own = Ownership::Populated;
    req.source = PopulateSource(n.sound.source, n.uid, materialised);  // 13-11: a File Sound's imported item
    const int v = rs.variation;
    if (v >= 0 && v < static_cast<int>(n.sound.frames.size())) {
        if (n.sound.mode == DetectMode::Takes) req.variation = v;
        else if (n.sound.mode == DetectMode::TakeMarkers || n.sound.mode == DetectMode::Silence)
            req.slice = SliceApplyFor(n.sound.frames[static_cast<size_t>(v)]);
    }
    req.gain_db = rs.gain_db;  // ResolveEvent already added the frame's dB
    req.blend_ms = rs.offset_ms;
    return req;
}

void RemovePopulated(EventSounds& ev)
{
    std::vector<SoundPlacement> keep;
    for (const SoundPlacement& p : ev.placed) {
        if (p.own == Ownership::Populated) RemoveIndexed(p.item_guid);
        else keep.push_back(p);
    }
    ev.placed = keep;
}

// One item: prune, draw, place / replace / remove, write its record. `events` counts the events
// handled.
void PopulateOne(MediaItem* item, const SoundStore& store, const PopOpts& o, SoundReport& rep, int* events)
{
    const ItemDetection det = DetectItem(item);
    if (det.status != ItemDetection::Status::Ok) return;
    const std::string guid = ItemGuidOf(item);
    const std::vector<Block>& blocks = det.rules.blocks;
    ItemSounds rec = ReadItemSounds(item);

    // P1: assignments of rules that no longer exist.
    std::set<std::string> live;
    for (const Block& b : blocks)
        if (!b.rid.empty()) live.insert(b.rid);
    const OrphanPrune pr = PruneOrphanRules(rec, live);
    for (const std::string& g : pr.remove_guids) RemoveIndexed(g);
    for (const RuleSounds& r : pr.rules) {  // Antho 2026-10-12: removed and listed, by every gesture
        const SoundLibrary* lib = FindLibrary(store, r.library);
        AddLine(rep, ReportKind::Detached, RuleGoneText(lib ? lib->name : r.library), "", guid, 0.0);
    }

    if (rec.seed == 0) rec.seed = RandomSeed();
    DrawState st;
    st.seed = rec.seed;
    std::set<std::string> blocked_rules;
    std::set<std::string> gone_rules;
    std::map<std::string, std::string> materialised;  // File Sound uid -> its imported item ("" = missing)
    std::map<std::string, std::string> materialise_why;  // File Sound uid -> why it was not imported

    // The events and their passes: the one list the follow (13-9) also reads (SoundEventsOf).
    const std::vector<FollowEvent> sound_events =
        SoundEventsOf(BuildEventList(det.events, det.rules.events, blocks.size()), blocks, det.map);
    for (const FollowEvent& e : sound_events) {
        const std::string& rid = e.rid;
        if (!e.enabled) continue;  // fix C 2026-10-12: a disabled rule's sounds are kept, muted by the follow
        if (!o.rid.empty() && rid != o.rid) continue;
        if (o.has_t && std::fabs(e.t - o.t) > kEventMatchSeconds) continue;
        EventSounds* ev = FindEventSounds(rec, rid, e.t);
        if (o.only_populated && (!ev || !EventIsPopulated(*ev))) continue;
        if (EventSkippedByPopulate(ev)) continue;  // attached / spread: left alone

        const RuleSounds*   rs = FindRuleSounds(rec, rid);
        const SoundLibrary* lib = rs && !rs->library.empty() ? FindLibrary(store, rs->library) : nullptr;
        if (!lib) {  // P8: library None removes the populated sounds
            const bool had = ev && EventIsPopulated(*ev);
            if (ev) RemovePopulated(*ev);
            if (rs && !rs->library.empty() && gone_rules.insert(rid).second)  // a deleted library: said
                AddLine(rep, ReportKind::Blocked,
                        had ? "Library gone: its placed sounds removed" : "Library gone: nothing placed", "", guid,
                        e.t);
            continue;
        }
        const LibraryCheck chk = CheckLibrary(store, *lib);
        if (chk.blocked) {
            if (blocked_rules.insert(rid).second)
                AddLine(rep, ReportKind::Blocked, "Library \"" + lib->name + "\" blocked: " + chk.reason, "", guid, e.t);
            continue;
        }
        if (!ev) {
            EventSounds fresh;
            fresh.rid = rid;
            fresh.t = e.t;
            rec.events.push_back(fresh);
            ev = &rec.events.back();
        }
        if (events) ++*events;

        // P9: a recorded item no longer in the project was deleted by hand.
        MarkDeletedPlacements(*ev, [](const std::string& g) { return FindItemByGuid(g) != nullptr; });

        // P10: the draw.
        st.lib = lib;
        const std::vector<SoundDraw> prev = ev->draws;
        EventDraw ed;
        ed.rid = rid;
        ed.t = ev->t;
        ed.prev = &prev;
        const std::string lib_uid = lib->uid;
        auto is_missing = [&materialised, &materialise_why, &lib_uid, &rep, &guid, &e](const SoundNode& n) {
            if (n.sound.source.kind == SoundSourceKind::File) {  // 13-11: imported once, then an Item
                auto it = materialised.find(n.uid);
                if (it == materialised.end()) {
                    std::string why, at;
                    it = materialised.emplace(n.uid, MaterialiseFileSound(nullptr, lib_uid, n.uid, &why, &at)).first;
                    if (it->second.empty() && !why.empty()) materialise_why[n.uid] = why;
                    if (!at.empty())  // 13-11 (B9): say where the muted library source sits
                        AddLine(rep, ReportKind::Info, "Library source \"" + NodeName(&n) + "\" imported on " + at,
                                n.uid, guid, e.t);
                }
                return it->second.empty();
            }
            return !FindItemByGuid(n.sound.source.item_guid);
        };
        const Resolution res = ResolveEvent(store, *lib, LayersFor(rec, rid, ev->t), MakeDrawHooks(st, ed, is_missing));
        std::vector<SoundDraw> draws = ed.out;
        for (const SoundDraw& d : prev) {  // a hand pick on a branch not walked now is kept
            bool have = false;
            for (const SoundDraw& x : draws)
                if (x.node == d.node) have = true;
            if (!have && d.hand) draws.push_back(d);
        }
        ev->draws = draws;
        if (res.blocked) {
            AddLine(rep, ReportKind::Blocked, res.reason.empty() ? std::string("blocked") : res.reason, "", guid, e.t);
            continue;
        }

        std::vector<double> passes;
        for (const FollowPass& fp : e.passes) passes.push_back(fp.project_t);
        std::vector<DesiredSlot>   desired;
        std::vector<const ResolvedSound*> desired_rs;
        std::set<std::string>      hold;  // missing / no active variation: placed copies are kept
        for (const ResolvedSound& s : res.sounds) {
            const SoundNode* n = FindNode(*lib, s.node);
            if (!n) continue;
            if (s.missing) {
                AddLine(rep, ReportKind::Missing,
                        n->sound.source.kind == SoundSourceKind::File
                            ? (materialise_why.count(n->uid) ? materialise_why[n->uid]
                                                             : "File not found: " + n->sound.source.asset.name)
                            : "Source item missing: " + NodeName(n),
                        n->uid, guid, e.t);
                hold.insert(n->uid);
                continue;
            }
            if (ed.no_active.count(n->uid)) {
                AddLine(rep, ReportKind::Missing, NodeName(n) + ": no active variation", n->uid, guid, e.t);
                hold.insert(n->uid);
                continue;
            }
            for (size_t p = 0; p < passes.size(); ++p) {
                DesiredSlot d;
                d.pass = static_cast<int>(p);
                d.node = n->uid;
                d.variation = s.variation;
                desired.push_back(d);
                desired_rs.push_back(&s);
            }
        }
        std::vector<ExistingSlot> existing;
        for (const SoundPlacement& p : ev->placed) {
            ExistingSlot x;
            x.node = p.node;
            x.variation = p.variation;
            x.own = p.own;
            if (p.own == Ownership::Populated && hold.count(p.node)) x.own = Ownership::Attached;  // planner: leave it
            if (x.own == Ownership::Populated)
                if (MediaItem* it = FindItemByGuid(p.item_guid)) {
                    double anchor = 0.0, sync = 0.0;  // the pile's anchor: one pass for the whole pile
                    PlacedPileAnchor(it, &anchor, &sync);
                    x.pass = NearestPassOfItem(passes, anchor, sync, BlendOffsetMs(*lib, p.node));
                }
            existing.push_back(x);
        }

        std::set<int>               erase;
        std::vector<SoundPlacement> added;
        for (const SlotStep& step : PlanEventSlots(desired, existing)) {
            if (step.act == SlotAction::Keep) continue;
            if (step.act == SlotAction::Remove) {
                for (int i : step.existing) {
                    RemoveIndexed(ev->placed[static_cast<size_t>(i)].item_guid);
                    erase.insert(i);
                }
                if (o.report_replaced && !step.existing.empty()) {
                    const std::string& was = ev->placed[static_cast<size_t>(step.existing.front())].node;
                    AddLine(rep, ReportKind::Replaced, NodeName(FindNode(*lib, was)) + " -> (none)", was, guid, e.t);
                }
                continue;
            }
            const DesiredSlot&   d = desired[static_cast<size_t>(step.desired)];
            const ResolvedSound& s = *desired_rs[static_cast<size_t>(step.desired)];
            const SoundNode*     n = FindNode(*lib, d.node);
            if (!n || d.pass < 0 || static_cast<size_t>(d.pass) >= passes.size()) continue;
            const double  pt = passes[static_cast<size_t>(d.pass)];
            PlaceRequest  req = RequestFor(item, rid, ev->t, pt, *n, s, &materialised);
            PlaceResult   r;
            if (step.act == SlotAction::Place) {
                r = PlaceSounds(nullptr, req);
            } else {
                // Replace in place, the user's nudge kept.
                const SoundPlacement& old = ev->placed[static_cast<size_t>(step.existing.front())];
                if (MediaItem* oi = FindItemByGuid(old.item_guid)) {
                    // A folder pile is placed from the folder's snap, its members offset from it:
                    // read back from the surviving member's tag (13-8).
                    const SoundLibrary& L = *lib;
                    double              anchor = 0.0, sync = 0.0;
                    PlacedPileAnchor(oi, &anchor, &sync);
                    req.offset_s = NudgeOf(anchor, pt, sync, BlendOffsetMs(L, old.node));
                }
                r = ReplacePlacement(nullptr, old.item_guid, req);
                for (size_t k = 0; k < step.existing.size(); ++k) {
                    const int i = step.existing[k];
                    if (k > 0 && r.ok) RemoveIndexed(ev->placed[static_cast<size_t>(i)].item_guid);
                    if (r.ok) erase.insert(i);
                }
                if (r.ok && step.act == SlotAction::ReplaceNode && o.report_replaced) {
                    const SoundNode* was = FindNode(*lib, old.node);
                    AddLine(rep, ReportKind::Replaced, NodeName(was) + " -> " + NodeName(n), n->uid, guid, e.t);
                }
            }
            if (!r.ok) {
                AddLine(rep, ReportKind::Missing, NodeName(n) + ": " + (r.error.empty() ? "not placed" : r.error), n->uid,
                        guid, e.t);
                continue;
            }
            if (g_items)
                for (size_t k = 0; k < r.item_guids.size() && k < r.items.size(); ++k)
                    (*g_items)[GuidKey(r.item_guids[k])] = r.items[k];
            CheckOwners(r.item_guids, guid, rep, n->uid, e.t);
            for (const std::string& g : r.item_guids) {
                SoundPlacement p;
                p.item_guid = g;
                p.node = n->uid;
                p.own = Ownership::Populated;
                p.offset_s = req.offset_s;
                p.variation = s.variation;
                added.push_back(p);
            }
        }
        std::vector<SoundPlacement> placed;
        for (size_t i = 0; i < ev->placed.size(); ++i)
            if (!erase.count(static_cast<int>(i))) placed.push_back(ev->placed[i]);
        placed.insert(placed.end(), added.begin(), added.end());
        ev->placed = placed;
    }

    ModifyItemSounds(item, nullptr, [&](ItemSounds& s) {
        s = rec;
        return true;
    });
}

// Gives every block a rid (P7), pooled, inside the caller's undo scope.
void EnsureRids(MediaItem* item)
{
    ItemRulesRead rr;
    if (!ReadItemRules(item, &rr) || !rr.valid) return;
    bool missing = false;
    for (const Block& b : rr.rules.blocks)
        if (b.rid.empty()) missing = true;
    if (!missing) return;
    std::string err;
    ModifyItemRules(item, "RAV: Set rule ids", [](ItemRules& r) { return EnsureRuleIds(&r); }, &err);
}

std::string RidOf(MediaItem* item, int block)
{
    ItemRulesRead rr;
    if (!ReadItemRules(item, &rr) || !rr.valid) return {};
    if (block < 0 || static_cast<size_t>(block) >= rr.rules.blocks.size()) return {};
    return rr.rules.blocks[static_cast<size_t>(block)].rid;
}

}  // namespace

void InitSoundPopulate(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_set_selected = reinterpret_cast<SetMediaItemSelectedFn>(get_func("SetMediaItemSelected"));
}

std::string ItemGuidOf(MediaItem* item)
{
    char buf[64] = {};
    if (!item || !GetSetMediaItemInfo_String(item, "GUID", buf, false)) return {};
    return buf;
}

MediaItem* FindItemByGuid(const std::string& guid)
{
    if (guid.empty()) return nullptr;
    if (g_items) {  // inside a gesture: its map; a miss (an item added outside it) is looked up and kept
        auto hit = g_items->find(GuidKey(guid));
        if (hit != g_items->end()) return hit->second;
        MediaItem* it = FindPlacedItem(guid);
        if (it) (*g_items)[GuidKey(guid)] = it;
        return it;
    }
    return FindPlacedItem(guid);
}

void SelectOnlyItem(MediaItem* item)
{
    if (!item || !f_set_selected) return;
    Main_OnCommand(kUnselectAllItems, 0);
    f_set_selected(item, true);
    UpdateArrange();
}

std::vector<MediaItem*> SelectedRavItems()
{
    std::vector<MediaItem*> out;
    const int n = CountSelectedMediaItems(nullptr);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetSelectedMediaItem(nullptr, i);
        if (it && RavTakeOf(it)) out.push_back(it);
    }
    return out;
}

std::vector<MediaItem*> PopulateTargets(const std::vector<MediaItem*>& items)
{
    std::vector<MediaItem*> rav;
    for (MediaItem* it : items)
        if (it && RavTakeOf(it)) rav.push_back(it);
    if (rav.empty()) return rav;
    return WithPoolMembers(rav);
}

SoundReport PopulateItemsCounted(const std::vector<MediaItem*>& items, int* events)
{
    SoundReport rep;
    if (events) *events = 0;
    try {
        const std::vector<MediaItem*> targets = PopulateTargets(items);
        if (targets.empty()) return rep;
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, "RAV: Populate sounds");
            ItemIndexScope index;
            PopOpts o;
            for (MediaItem* it : targets) PopulateOne(it, store, o, rep, events);
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

SoundReport PopulateItems(const std::vector<MediaItem*>& items)
{
    return PopulateItemsCounted(items, nullptr);
}

SoundReport ReshuffleItems(const std::vector<MediaItem*>& items)
{
    SoundReport rep;
    try {
        const std::vector<MediaItem*> targets = PopulateTargets(items);
        if (targets.empty()) return rep;
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, "RAV: Reshuffle sounds");
            ItemIndexScope index;
            PopOpts o;
            o.only_populated = true;
            o.report_replaced = false;
            for (MediaItem* it : targets) {
                const uint64_t seed = RandomSeed();
                ModifyItemSounds(it, nullptr, [seed](ItemSounds& s) {
                    Reshuffle(s, seed);
                    return true;
                });
                PopulateOne(it, store, o, rep, nullptr);
            }
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

SoundReport SetRuleLibrary(MediaItem* item, int block, const std::string& library_uid)
{
    SoundReport rep;
    try {
        if (!item || !RavTakeOf(item)) return rep;
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, "RAV: Set rule library");
            ItemIndexScope index;
            EnsureRids(item);
            const std::string rid = RidOf(item, block);
            if (rid.empty()) return rep;
            const std::vector<MediaItem*> members = WithPoolMembers({item});
            for (MediaItem* m : members)
                ModifyItemSounds(m, nullptr, [&](ItemSounds& s) {
                    RuleSounds* r = FindRuleSounds(s, rid);
                    if (!r) {
                        if (library_uid.empty()) return false;
                        RuleSounds fresh;
                        fresh.rid = rid;
                        s.rules.push_back(fresh);
                        r = &s.rules.back();
                    }
                    if (r->library == library_uid) return false;
                    r->library = library_uid;
                    return true;
                });
            PopOpts o;
            o.only_populated = true;
            o.rid = rid;
            for (MediaItem* m : members) PopulateOne(m, store, o, rep, nullptr);
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

SoundReport SetSoundValue(MediaItem* item, SoundValueLevel level, int block, double t, const std::string& group_uid,
                          const std::string& value_uid)
{
    SoundReport rep;
    try {
        if (!item || !RavTakeOf(item) || group_uid.empty()) return rep;
        const SoundStore store = GetSoundStore(nullptr);
        {
            SoundUndoScope undo(nullptr, "RAV: Set sound value");
            ItemIndexScope index;
            std::string rid;
            if (level != SoundValueLevel::Item) {
                EnsureRids(item);
                rid = RidOf(item, block);
                if (rid.empty()) return rep;
            }
            const std::vector<MediaItem*> members = WithPoolMembers({item});
            for (MediaItem* m : members)
                ModifyItemSounds(m, nullptr, [&](ItemSounds& s) {
                    switch (level) {
                    case SoundValueLevel::Item: return SetValueIn(s.item_values, group_uid, value_uid);
                    case SoundValueLevel::Rule: {
                        RuleSounds* r = FindRuleSounds(s, rid);
                        if (!r) {
                            if (value_uid.empty()) return false;
                            RuleSounds fresh;
                            fresh.rid = rid;
                            s.rules.push_back(fresh);
                            r = &s.rules.back();
                        }
                        return SetValueIn(r->values, group_uid, value_uid);
                    }
                    case SoundValueLevel::Event: {
                        EventSounds* e = FindEventSounds(s, rid, t);
                        if (!e) {
                            if (value_uid.empty()) return false;
                            EventSounds fresh;
                            fresh.rid = rid;
                            fresh.t = t;
                            s.events.push_back(fresh);
                            e = &s.events.back();
                        }
                        return SetValueIn(e->overrides, group_uid, value_uid);
                    }
                    }
                    return false;
                });
            PopOpts o;
            o.only_populated = true;
            o.rid = rid;
            o.has_t = level == SoundValueLevel::Event;
            o.t = t;
            for (MediaItem* m : members) PopulateOne(m, store, o, rep, nullptr);
        }
        UpdateArrange();
    } catch (...) {
        AddLine(rep, ReportKind::Blocked, "Internal error: the gesture stopped part way (Ctrl+Z undoes it)", "", "", 0.0);
    }
    return rep;
}

int CountPopulateEvents(const std::vector<MediaItem*>& items)
{
    int n = 0;
    try {
        const SoundStore& store = GetSoundStore(nullptr);
        if (store.libraries.empty()) return 0;
        for (MediaItem* it : PopulateTargets(items)) {
            const ItemDetection det = DetectItem(it);
            if (det.status != ItemDetection::Status::Ok) continue;
            const ItemSounds rec = ReadItemSounds(it);
            std::vector<char> ok(det.rules.blocks.size(), 0);
            for (size_t b = 0; b < det.rules.blocks.size(); ++b) {
                const RuleSounds* rs = FindRuleSounds(rec, det.rules.blocks[b].rid);
                if (!det.rules.blocks[b].enabled || det.rules.blocks[b].rid.empty() || !rs || rs->library.empty())
                    continue;
                const SoundLibrary* lib = FindLibrary(store, rs->library);
                ok[b] = lib && !CheckLibrary(store, *lib).blocked ? 1 : 0;
            }
            std::vector<int> evb;
            for (const ShownEvent& e : BuildEventList(det.events, det.rules.events, det.rules.blocks.size())) {
                if (e.kind != ShownKind::Detected && e.kind != ShownKind::User) continue;
                if (e.block >= 0 && static_cast<size_t>(e.block) < det.rules.blocks.size() &&
                    EventSkippedByPopulate(FindEventSounds(rec, det.rules.blocks[static_cast<size_t>(e.block)].rid, e.t)))
                    continue;  // the same test as PopulateOne
                evb.push_back(e.block);
            }
            n += CountEvents(evb, ok);
        }
    } catch (...) {
    }
    return n;
}

}  // namespace rav

#endif  // _WIN32
