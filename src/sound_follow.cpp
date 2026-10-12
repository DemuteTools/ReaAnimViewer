// SPDX-License-Identifier: MIT
//
// See sound_follow.h (spec 13-9). The REAPER side of the follow: reads the facts, plans each
// animation item (sound_follow_plan.h), applies the plan through the placement engine (13-6),
// keeps the follow report, and draws the menu's Sounds section.
//
// F4: the follow writes create no undo point (the user's gesture is the undo point; the next
// tick converges again). Repopulate (reset) is a gesture: one SoundUndoScope.

#include "sound_follow.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "event_list.h"
#include "imgui.h"
#include "item_rules.h"
#include "picked_gestures.h"
#include "shortcuts.h"
#include "sound_follow_plan.h"
#include "sound_place.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "sound_populate_plan.h"
#include "sound_record.h"
#include "sound_tracks_plan.h"
#include "tagging_session.h"
#include "undo_guard.h"
#include "tagging_signal.h"
#include "ui_flow.h"
#include "ui_theme.h"

namespace rav {

namespace {

using SetMediaItemInfo_ValueFn = bool (*)(MediaItem* item, const char* parmname, double newvalue);
SetMediaItemInfo_ValueFn f_set_item = nullptr;
// Fix C review 2: REAPER's language pack (optional: names stay English without it).
using LocalizeStringFn = const char* (*)(const char* src_string, const char* section, int flags);
LocalizeStringFn f_localize = nullptr;

// The localized forms of REAPER's undo names (those that differ from the English), read once.
std::vector<std::string> LocalizedNames(std::initializer_list<const char*> names)
{
    std::vector<std::string> out;
    if (!f_localize) return out;
    for (const char* n : names)
        for (const char* sec : {"undo", "common", "accel_sec"}) {
            const char* l = nullptr;
            try {
                l = f_localize(n, sec, 0);
            } catch (...) {
                l = nullptr;
            }
            if (!l || !*l || std::strcmp(l, n) == 0) continue;
            if (std::find(out.begin(), out.end(), std::string(l)) == out.end()) out.emplace_back(l);
        }
    return out;
}

const std::vector<std::string>& CutNames()
{
    static const std::vector<std::string> v = LocalizedNames(
        {"Cut items", "Cut selected area of items", "Cut items/tracks/envelope points", "Ripple cut"});
    return v;
}

const std::vector<std::string>& SelectionNames()
{
    static const std::vector<std::string> v = LocalizedNames(
        {"Change media item selection", "Unselect all items", "Select all items", "Change track selection",
         "Change selection"});
    return v;
}

constexpr size_t kReportMax = 200;
constexpr char   kKeepDrawPref[] = "sound_copy_keeps_draw";

// What the follow last saw of an owner (an animation item with sounds).
struct OwnerSnap {
    std::string                  guid;       // as REAPER writes it
    std::string                  rules_raw;  // its rules record text
    std::string                  path;       // its file
    double                       pos = 0.0;
    std::vector<SoundPlacement>  placed;
    std::vector<FollowItemFacts> facts;      // its placed items then
    // Fix C 2026-10-12: what it was before it was moved (cut and paste), latest first: a paste
    // undone leaves its items tagged for the GUID it had, so they stay its own.
    std::vector<OwnerSnap>       prev;
};
constexpr size_t kPrevMax = 4;
constexpr size_t kRecentMax = 8;
// Fix C 2026-10-12: an owner cut, waiting for its paste while the cut is REAPER's last undo point.
struct PendingGone {
    OwnerSnap                snap;
    std::string              undo_desc;  // the cut's undo point name
    std::vector<std::string> muted;      // its sound items left behind, muted by the follow meanwhile
};
struct FollowProject {
    bool                               scanned = false;
    std::set<std::string>              seen;     // item GUIDs (upper case)
    std::map<std::string, OwnerSnap>   owners;   // owner GUID (upper) -> what was last seen
    std::map<std::string, PendingGone> pending;  // owner GUID (upper) -> gone, not yet handled
    // Fix C review 2: owners whose deletion was handled, latest first: a copy pasted from one of
    // them later finds the sound items pasted with it.
    std::deque<OwnerSnap>              recent;
};

void Remember(FollowProject& fp, const OwnerSnap& snap)
{
    fp.recent.push_front(snap);
    while (fp.recent.size() > kRecentMax) fp.recent.pop_back();
}
std::map<ReaProject*, FollowProject>          g_follow;
std::map<ReaProject*, std::deque<ReportLine>> g_report;

struct ReadItem {
    MediaItem*      item = nullptr;
    FollowItemFacts facts;
};

std::string Upper(std::string s)
{
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return s;
}

// ItemGuidOf comes from sound_populate.h (13-8, merged first; same body).

std::string TrackGuidOf(MediaItem* item)
{
    MediaTrack* tr = item ? GetMediaItem_Track(item) : nullptr;
    char        buf[64] = {};
    if (!tr || !GetSetMediaTrackInfo_String(tr, "GUID", buf, false)) return {};
    return buf;
}

std::string ItemExt(MediaItem* item, const char* key)
{
    std::string buf(4096, '\0');
    if (!GetSetMediaItemInfo_String(item, key, &buf[0], false)) return {};
    buf.resize(std::strlen(buf.c_str()));
    return buf;
}

// Fix 2026-10-12: the follow's events are Populate's (SoundEventsOf over the live detection, the
// marker mirror's list), not the record's last Commit. Detection is the costly part: it runs again
// only when what it reads changed (the item's rules record, its file) or after kDetectKeepS (the
// file re-exported in place, a role mapping changed in its own file). The passes are recomputed
// every tick from the item's position, length, loop, start offset and rate (cheap).
constexpr double kDetectKeepS = 5.0;
struct DetectedEvents {
    std::string                           rules_raw;
    std::string                           path;
    std::chrono::steady_clock::time_point when;
    bool                                  ok = false;
    double                                clip_len = 0.0;
    std::vector<Event>                    events;  // DetectItem's detections
    ItemRules                             rules;   // the record they go with
};
std::map<ReaProject*, std::map<std::string, DetectedEvents>> g_detected;  // owner GUID (upper) -> its detection

// The item's sound events now, as PopulateOne builds them. False when its detection does not run
// (nothing is decided then, as Populate places nothing).
bool FollowEventsOf(ReaProject* proj, MediaItem* item, const std::string& key, const ItemRulesRead& rr,
                    std::vector<FollowEvent>* out)
{
    MediaItem_Take*   take = RavTakeOf(item);
    const std::string path = take ? AnimPathOf(take) : std::string();
    const auto           now = std::chrono::steady_clock::now();
    DetectedEvents&      d = g_detected[proj][key];
    const bool fresh = d.when.time_since_epoch().count() != 0 && d.rules_raw == rr.raw && d.path == path &&
                       std::chrono::duration<double>(now - d.when).count() < kDetectKeepS;
    if (!fresh) {
        const ItemDetection det = DetectItem(item);
        d.rules_raw = rr.raw;
        d.path = path;
        d.when = now;
        d.ok = det.status == ItemDetection::Status::Ok;
        d.clip_len = det.map.clip_len;
        d.events = d.ok ? det.events : std::vector<Event>();
        d.rules = d.ok ? det.rules : ItemRules();
    }
    if (!d.ok) return false;
    const ItemClipMap map = ItemClipMapOf(item, d.clip_len);  // DetectItem's own map, now
    *out = SoundEventsOf(BuildEventList(d.events, d.rules.events, d.rules.blocks.size()), d.rules.blocks, map);
    return true;
}

// Each placed node's Blend offset (ms), for finding a sound's pass as Populate does.
std::vector<std::pair<std::string, double>> BlendOffsets(const SoundStore& store, const ItemSounds& rec,
                                                         const std::vector<Block>& blocks, const SoundCharacter* ch)
{
    std::vector<std::pair<std::string, double>> out;
    for (const EventSounds& ev : rec.events) {
        const Block* blk = nullptr;
        for (const Block& b : blocks)
            if (!b.rid.empty() && b.rid == ev.rid) blk = &b;
        if (!blk) continue;
        const EffectiveAssign ea = ResolveBlock(store, rec, ch, *blk);  // 13-15a: rule, Character, Project
        const SoundLibrary*   lib = ea.library.empty() ? nullptr : FindLibrary(store, ea.library);
        if (!lib) continue;
        for (const SoundPlacement& p : ev.placed) {
            bool have = false;
            for (const auto& b : out) have |= b.first == p.node;
            if (!have) out.emplace_back(p.node, BlendOffsetMs(*lib, p.node));
        }
    }
    return out;
}

bool HasSounds(const ItemSounds& s)
{
    return !s.rules.empty() || !s.events.empty() || !s.item_values.empty();
}

void Report(ReaProject* proj, const std::vector<ReportLine>& lines)
{
    if (lines.empty()) return;
    std::deque<ReportLine>& r = g_report[proj];
    for (const ReportLine& l : lines) r.push_back(l);
    while (r.size() > kReportMax) r.pop_front();
}

std::vector<FollowItemFacts> FactsFor(const std::map<std::string, ReadItem>& all, const std::vector<SoundPlacement>& placed)
{
    std::vector<FollowItemFacts> out;
    for (const SoundPlacement& p : placed) {
        const auto it = all.find(Upper(p.item_guid));
        if (it != all.end()) out.push_back(it->second.facts);
    }
    return out;
}

std::vector<SoundPlacement> AllPlacements(const ItemSounds& rec)
{
    std::vector<SoundPlacement> out;
    for (const EventSounds& ev : rec.events)
        for (const SoundPlacement& p : ev.placed) out.push_back(p);
    return out;
}

// Applies the actions; true when an item moved, was removed or (un)muted. An item whose move or
// (un)mute failed keeps its old store (the next tick plans it again instead of reading a nudge).
bool Apply(ReaProject* proj, const FollowPlan& plan, const std::map<std::string, ReadItem>& all)
{
    bool                  arrange = false;
    std::set<std::string> failed;
    for (const FollowAction& a : plan.actions) {
        try {
            const auto       it = all.find(Upper(a.item_guid));
            MediaItem* const item = it != all.end() ? it->second.item : nullptr;
            switch (a.kind) {
            case FollowKind::Move:
                if (MovePlacement(proj, a.item_guid, a.project_t)) arrange = true;
                else failed.insert(Upper(a.item_guid));
                break;
            case FollowKind::Remove:
                arrange |= RemovePlacement(proj, a.item_guid);
                break;
            case FollowKind::Detach:
                if (item) ClearPlacedTag(item);
                break;
            case FollowKind::Mute:
            case FollowKind::Unmute:
                if (item && f_set_item && f_set_item(item, "B_MUTE", a.kind == FollowKind::Mute ? 1.0 : 0.0)) arrange = true;
                else failed.insert(Upper(a.item_guid));
                break;
            case FollowKind::Store:
                if (item && !failed.count(Upper(a.item_guid))) {
                    std::string text = FormatFollowStore(a.store);
                    GetSetMediaItemInfo_String(item, kFollowStoreKey, &text[0], true);
                }
                break;
            case FollowKind::Retag:
                if (item) {
                    PlacedTag tag;
                    if (ReadPlacedTag(item, &tag)) {
                        tag.t = a.tag_t;
                        if (!a.tag_owner.empty()) tag.owner = a.tag_owner;
                        WritePlacedTag(item, tag);
                    }
                }
                break;
            case FollowKind::Forget:
                break;
            }
        } catch (...) {
        }
    }
    return arrange;
}

// The GUIDs an owner's items may be tagged for: its own and those it had before a move.
std::vector<std::string> OwnerGuids(const OwnerSnap& s)
{
    std::vector<std::string> out{s.guid};
    for (const OwnerSnap& p : s.prev) out.push_back(p.guid);
    return out;
}

std::vector<std::string> PlacedGuids(const std::vector<SoundPlacement>& placed)
{
    std::vector<std::string> out;
    for (const SoundPlacement& p : placed) out.push_back(p.item_guid);
    return out;
}

void SetMute(const std::map<std::string, ReadItem>& all, const std::string& guid, bool mute)
{
    const auto it = all.find(Upper(guid));
    if (it != all.end() && f_set_item) f_set_item(it->second.item, "B_MUTE", mute ? 1.0 : 0.0);
}

// F9: a gone owner's items (and those of the GUIDs it had, unless that item is back) handled.
bool HandleGone(ReaProject* proj, const OwnerSnap& snap, const std::map<std::string, ReadItem>& all)
{
    std::vector<SoundPlacement> placed;
    std::set<std::string>       have;
    std::vector<std::string>    also;
    auto add = [&](const OwnerSnap& s) {
        for (const SoundPlacement& p : s.placed)
            if (have.insert(Upper(p.item_guid)).second) placed.push_back(p);
    };
    add(snap);
    for (const OwnerSnap& p : snap.prev) {
        if (all.count(Upper(p.guid))) continue;  // that item is back (undo): its items are its own
        also.push_back(p.guid);
        add(p);
    }
    const FollowPlan plan = PlanOwnerGone(snap.guid, placed, FactsFor(all, placed), also);
    const bool       arrange = Apply(proj, plan, all);
    Report(proj, plan.report);
    return arrange;
}

ReaProject* CurrentProject()
{
    return EnumProjects(-1, nullptr, 0);
}

// The selected animation items that have a sound record (F10).
std::vector<MediaItem*> RepopulateItems(ReaProject* proj, int* placed_out)
{
    std::vector<MediaItem*> out;
    int                     placed = 0;
    const int               n = CountSelectedMediaItems(proj);
    for (int i = 0; i < n; ++i) {
        MediaItem* item = GetSelectedMediaItem(proj, i);
        if (!item || !RavTakeOf(item)) continue;
        const ItemSounds rec = ReadItemSounds(item);
        if (!HasSounds(rec)) continue;
        out.push_back(item);
        for (const EventSounds& ev : rec.events)
            for (const SoundPlacement& p : ev.placed) placed += p.own == Ownership::Populated;
    }
    if (placed_out) *placed_out = placed;
    return out;
}

void Repopulate(ReaProject* proj)
{
    int                           placed = 0;
    const std::vector<MediaItem*> items = RepopulateItems(proj, &placed);
    if (items.empty()) return;
    SoundUndoScope scope(proj, "RAV: Repopulate sounds (reset)");
    const uint64_t salt = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    for (MediaItem* item : items) {
        const std::string guid = ItemGuidOf(item);
        ModifyItemSounds(item, nullptr, [&](ItemSounds& rec) {
            for (EventSounds& ev : rec.events) {
                std::vector<SoundPlacement> kept;
                for (const SoundPlacement& p : ev.placed) {
                    if (p.own == Ownership::Populated) RemovePlacement(proj, p.item_guid);
                    if (p.own == Ownership::Populated || p.own == Ownership::Deleted) continue;
                    kept.push_back(p);
                }
                ev.placed = std::move(kept);
                ev.draws.clear();
            }
            rec.seed = FollowGuidHash(guid) ^ salt;
            if (rec.seed == 0) rec.seed = 1;
            return true;
        });
    }
    Report(proj, PopulateItems(items).lines);  // nested in the scope: one undo point
    UpdateArrange();
}


const char* KindText(ReportKind k)
{
    switch (k) {
    case ReportKind::Missing: return "Missing";
    case ReportKind::Replaced: return "Replaced";
    case ReportKind::Detached: return "Detached";
    case ReportKind::Blocked: return "Blocked";
    case ReportKind::Info: return "Note";
    }
    return "";
}

bool g_confirm_reset = false;

// 13-13: the Tracks menu only picks; the store gesture runs after the menu (picked_gestures.h).
PickedGestures g_track_picks;

// The Tracks row: where placed sounds land (the project's TrackLayout), one undo point a change.
void DrawTracksRow(ReaProject* proj)
{
    static const char* const kShort[3] = {"Off", "Per path", "Folders"};
    static const char* const kRows[3] = {"Source or Lands-in track", "One track per Switch path", "Folders by Switch"};
    static const char* const kHints[3] = {nullptr, "Run-Shoes-Concrete", "Run / Shoes / Concrete"};
    static const TrackLayout kLayouts[3] = {TrackLayout::Off, TrackLayout::Flat, TrackLayout::Tree};
    const TrackLayout        cur = TrackLayoutOf(GetSoundStore(proj));
    const int                sel = cur == TrackLayout::Flat ? 1 : cur == TrackLayout::Tree ? 2 : 0;
    ui::LabelRow("Tracks", ui::Dp(90.0f));
    const std::string label = std::string(kShort[sel]) + " \xE2\x96\xBE##sndtracks";
    if (ui::SolidButton(label.c_str())) ImGui::OpenPopup("##sndtracks");
    ui::ItemTipText("Applies to new placements. Repopulate (reset) moves the existing ones.");
    if (ui::BeginMenuPopup("##sndtracks")) {
        ui::MenuCaption("PLACED SOUNDS LAND ON");
        for (int i = 0; i < 3; ++i)
            if (ImGui::MenuItem(kRows[i], kHints[i], i == sel) && i != sel) {
                const TrackLayout to = kLayouts[i];
                const std::string undo = std::string("RAV: Sound tracks: ") + kShort[i];
                g_track_picks.Pick([proj, to, undo]() {
                    ModifySoundStore(proj, undo.c_str(), [to](SoundStore& st) { return SetTrackLayout(st, to); });
                });
            }
        ui::EndMenuPopup();
    }
    g_track_picks.Run();  // after the menu: the gesture replaces the store
}

}  // namespace

void InitSoundFollow(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_set_item = reinterpret_cast<SetMediaItemInfo_ValueFn>(get_func("SetMediaItemInfo_Value"));
    f_localize = reinterpret_cast<LocalizeStringFn>(get_func("LocalizeString"));
}

void SoundFollowTick(ReaProject* proj)
{
    try {
        if (!proj) return;
        FollowProject& fp = g_follow[proj];

        // One pass over the items: the facts of every item.
        std::map<std::string, ReadItem> all;
        std::vector<MediaItem*>         anim_items;
        const int                       n = CountMediaItems(proj);
        for (int i = 0; i < n; ++i) {
            MediaItem* item = GetMediaItem(proj, i);
            if (!item) continue;
            ReadItem r;
            r.item = item;
            FollowItemFacts& f = r.facts;
            f.item_guid = ItemGuidOf(item);
            if (f.item_guid.empty()) continue;
            f.present = true;
            f.pos = GetMediaItemInfo_Value(item, "D_POSITION");
            f.track_guid = TrackGuidOf(item);
            f.muted = GetMediaItemInfo_Value(item, "B_MUTE") != 0.0;
            f.sync_offset = GetMediaItemInfo_Value(item, "D_SNAPOFFSET");
            PlacedTag tag;
            if (ReadPlacedTag(item, &tag)) {
                f.has_tag = true;
                f.tag_owner = tag.owner;
                f.tag_t = tag.t;
                f.tag_rid = tag.rid;
                f.tag_node = tag.node;
                f.tag_has_anchor = tag.has_anchor;
                f.tag_ao = tag.ao;
                f.tag_sy = tag.sy;
            }
            f.has_store = ParseFollowStore(ItemExt(item, kFollowStoreKey), &f.store);
            if (RavTakeOf(item)) anim_items.push_back(item);
            all[Upper(f.item_guid)] = r;
        }

        // The animation items and their records; every placed GUID of every record.
        struct Anim {
            MediaItem*    item = nullptr;
            std::string   guid, key, path;
            bool          appeared = false;
            double        pos = 0.0;
            double        len = 0.0;
            ItemSounds    rec;
            bool          rules_ok = false;
            ItemRulesRead rr;
        };
        std::vector<Anim>     anims;
        std::set<std::string> placed_guids;
        // Fix C review 2: an item gone and back (undo, redo) appears again.
        std::vector<std::string> anim_keys;
        for (MediaItem* item : anim_items) anim_keys.push_back(Upper(ItemGuidOf(item)));
        const std::vector<bool> appeared = UpdateSeenOwners(&fp.seen, anim_keys, fp.scanned);
        for (size_t ai = 0; ai < anim_items.size(); ++ai) {
            MediaItem* const item = anim_items[ai];
            try {
                Anim a;
                a.item = item;
                a.guid = ItemGuidOf(item);
                a.key = Upper(a.guid);
                a.appeared = appeared[ai];
                a.rec = ReadItemSounds(item);
                for (const EventSounds& ev : a.rec.events)
                    for (const SoundPlacement& p : ev.placed) placed_guids.insert(Upper(p.item_guid));
                if (!HasSounds(a.rec)) continue;
                a.rules_ok = ReadItemRules(item, &a.rr) && a.rr.valid;
                MediaItem_Take* take = RavTakeOf(item);
                a.path = take ? AnimPathOf(take) : std::string();
                a.pos = GetMediaItemInfo_Value(item, "D_POSITION");
                a.len = GetMediaItemInfo_Value(item, "D_LENGTH");
                anims.push_back(std::move(a));
            } catch (...) {
            }
        }
        // Fix C 2026-10-12: tagged sound items in no record (copied or pasted with an animation item).
        std::vector<FollowItemFacts> strays;
        for (const auto& kv : all)
            if (kv.second.facts.has_tag && !placed_guids.count(kv.first)) strays.push_back(kv.second.facts);

        // Fix C 2026-10-12: owners gone since the last tick. Matched with an item that appeared
        // (below), it moved; else a cut waits for its paste, any other deletion is handled now.
        const char* const  top_read = Undo_CanUndo2(proj);
        const std::string  top_desc = top_read ? top_read : "";
        const char* const  top_undo = top_read ? top_desc.c_str() : nullptr;  // a copy: Apply may write
        std::map<std::string, PendingGone> gone_now;  // owner GUID (upper) -> gone this tick
        for (auto it = fp.owners.begin(); it != fp.owners.end();) {
            if (all.count(it->first)) {
                ++it;
                continue;
            }
            gone_now[it->first] = PendingGone{it->second, top_desc, {}};
            it = fp.owners.erase(it);
        }
        // A gone owner (each GUID it had) and one that appeared with the same rules record, file
        // and placed items, none of them another owner's: moved.
        struct Moved {
            std::string key;   // the gone owner (fp.pending or gone_now)
            size_t      prev;  // which of its snaps matched: 0 = itself, k = prev[k - 1]
        };
        std::map<std::string, Moved> moved;  // new key -> the gone owner
        {
            std::vector<MoveCandidate> gone, came;
            std::vector<Moved>         gone_ids;
            std::vector<std::string>   came_keys;
            auto add_gone = [&](const std::string& key, const OwnerSnap& s) {
                const std::vector<std::string>        owners = OwnerGuids(s);
                std::vector<std::vector<std::string>> sets;
                for (size_t k = 0; k <= s.prev.size(); ++k) {
                    const OwnerSnap&               id = k == 0 ? s : s.prev[k - 1];
                    const std::vector<std::string> placed = PlacedGuids(id.placed);
                    bool                           dup = false;  // the same items as a later snap: that one
                    for (const auto& seen : sets) dup |= seen == placed;
                    if (dup) continue;
                    sets.push_back(placed);
                    gone.push_back({id.guid, id.rules_raw, id.path, placed, owners});
                    gone_ids.push_back({key, k});
                }
            };
            for (const auto& kv : fp.pending) add_gone(kv.first, kv.second.snap);
            for (const auto& kv : gone_now) add_gone(kv.first, kv.second.snap);
            for (const Anim& a : anims)
                if (a.appeared && a.rules_ok) {
                    MoveCandidate c{a.guid, a.rr.raw, a.path, PlacedGuids(AllPlacements(a.rec)), {}};
                    for (const std::string& g : c.placed) {
                        const auto it = all.find(Upper(g));
                        if (it != all.end() && it->second.facts.has_tag) c.owners.push_back(it->second.facts.tag_owner);
                    }
                    came.push_back(c);
                    came_keys.push_back(a.key);
                }
            std::set<std::string> taken;
            for (const auto& pr : MatchMovedOwners(gone, came)) {
                const Moved& g = gone_ids[pr.first];
                if (!taken.insert(g.key).second) continue;  // one paste per gone owner
                moved[came_keys[pr.second]] = g;
            }
        }

        const bool               keep_draw = LoadPrefFloat(kKeepDrawPref, 0.0f) != 0.0f;
        const SoundStore&        store = GetSoundStore(proj);
        std::vector<std::pair<std::string, std::string>> libs;
        for (const SoundLibrary& l : store.libraries) libs.emplace_back(l.uid, l.name);
        std::vector<std::pair<std::string, double>> anim_pos;
        for (MediaItem* item : anim_items) anim_pos.emplace_back(ItemGuidOf(item), GetMediaItemInfo_Value(item, "D_POSITION"));
        // Fix C review 2: owners no longer in the project, as last seen (a copy of a gone source).
        std::vector<FollowOwnerMemory> remembered;
        {
            auto add1 = [&](const OwnerSnap& x) {
                if (x.facts.empty() || all.count(Upper(x.guid))) return;
                remembered.push_back(FollowOwnerMemory{x.guid, x.pos, x.facts});
            };
            auto add = [&](const OwnerSnap& x) {
                add1(x);
                for (const OwnerSnap& q : x.prev) add1(q);
            };
            for (const auto& kv : gone_now) add(kv.second.snap);
            for (const auto& kv : fp.pending) add(kv.second.snap);
            for (const auto& kv : fp.owners) add(kv.second);
            for (const OwnerSnap& x : fp.recent) add(x);
        }

        bool                    arrange = false;
        std::vector<MediaItem*> copies;
        std::set<std::string>   owners_now;
        for (const Anim& an : anims) {
            try {
                MediaItem* const   item = an.item;
                const std::string& guid = an.guid;
                const std::string& key = an.key;
                const ItemSounds&  rec = an.rec;
                owners_now.insert(key);
                OwnerSnap snap;
                snap.guid = guid;
                snap.path = an.path;
                snap.pos = an.pos;
                snap.rules_raw = an.rr.raw;
                const auto was_owner = fp.owners.find(key);
                if (was_owner != fp.owners.end()) snap.prev = was_owner->second.prev;  // the GUIDs it had
                snap.placed = AllPlacements(rec);
                // Fix C 2026-10-12: moved (cut and paste): taken from the gone owner before anything
                // else, so it is never handled as gone even when this item's detection does not run.
                std::string                  mv_from;
                std::vector<std::string>     mv_also;
                std::vector<FollowItemFacts> mv_items;
                double                       mv_delta = 0.0;
                bool                         appeared_now = an.appeared;
                const auto                   mv = moved.find(key);
                if (mv != moved.end()) {
                    auto        pit = fp.pending.find(mv->second.key);
                    const bool  was_pending = pit != fp.pending.end();
                    PendingGone pg = was_pending ? pit->second : gone_now[mv->second.key];
                    if (was_pending) fp.pending.erase(pit);
                    else gone_now.erase(mv->second.key);
                    // Fix C review 2: matched with what it was itself (a cut undone): back, nothing
                    // moved. With a GUID it had before (a paste redone): moved from that one.
                    const bool       self = mv->second.key == key;
                    const OwnerSnap& id = mv->second.prev == 0 ? pg.snap : pg.snap.prev[mv->second.prev - 1];
                    if (!SameFollowGuid(id.guid, guid)) {
                        mv_from = id.guid;
                        for (const std::string& g : OwnerGuids(pg.snap))
                            if (!SameFollowGuid(g, id.guid) && !SameFollowGuid(g, guid)) mv_also.push_back(g);
                        mv_items = id.facts;
                        mv_delta = an.pos - id.pos;
                    } else {
                        appeared_now = false;
                    }
                    // Its sound items the follow muted while the cut waited: back as they were.
                    for (const std::string& g : pg.muted) {
                        SetMute(all, g, false);
                        auto it = all.find(Upper(g));
                        if (it != all.end()) it->second.facts.muted = false;
                    }
                    // Kept, so a paste undone leaves its items to this owner (latest first).
                    std::vector<OwnerSnap> had;
                    if (!self) {
                        had.push_back(pg.snap);
                        had.back().prev.clear();
                    }
                    for (const OwnerSnap& p : pg.snap.prev) had.push_back(p);
                    snap.prev.clear();
                    for (const OwnerSnap& p : had)
                        if (!SameFollowGuid(p.guid, guid)) snap.prev.push_back(p);
                    if (snap.prev.size() > kPrevMax) snap.prev.resize(kPrevMax);
                }
                snap.facts = FactsFor(all, snap.placed);
                if (!an.rules_ok) {
                    fp.owners[key] = snap;
                    continue;  // unreadable rules: nothing is decided
                }
                FollowInput in;
                if (!FollowEventsOf(proj, item, key, an.rr, &in.events)) {
                    fp.owners[key] = snap;
                    continue;  // its detection does not run: nothing is decided
                }
                in.owner_guid = guid;
                in.rec = rec;
                const ItemRigInfo rig = ItemRig(item);
                in.blend_ms = BlendOffsets(store, rec, an.rr.rules.blocks, CharacterFor(store, rig.skeleton, rig.model_file));
                for (const auto& b : an.rr.rules.blocks) {
                    if (b.rid.empty()) continue;
                    in.rule_rids.push_back(b.rid);
                    if (!b.enabled) in.disabled_rids.push_back(b.rid);
                }
                in.items = snap.facts;
                in.library_names = libs;
                in.appeared = appeared_now;
                in.keep_draw = keep_draw;
                in.copy_seed = FollowGuidHash(guid);
                in.owner_pos = an.pos;
                in.moved_from = mv_from;
                in.moved_also = mv_also;
                in.moved_items = mv_items;
                in.moved_delta = mv_delta;
                if (appeared_now) {
                    in.strays = strays;
                    in.owner_positions = anim_pos;
                    in.owner_len = an.len;
                    in.remembered = remembered;
                }
                const FollowPlan plan = PlanItemFollow(in);
                arrange |= Apply(proj, plan, all);
                if (plan.record_changed) {
                    const ItemSounds next = plan.record;
                    ModifyItemSounds(item, nullptr, [&](ItemSounds& r) {
                        r = next;
                        return true;
                    });
                }
                Report(proj, plan.report);
                snap.placed = AllPlacements(plan.record);
                snap.facts = FactsFor(all, snap.placed);
                FactsAfterPlan(plan, &snap.facts);  // positions as this tick's moves left them
                fp.owners[key] = snap;
                if (plan.populate_copy) copies.push_back(item);
            } catch (...) {
            }
        }

        // Fix C review 2: a pending owner whose item is back (undo) and was not matched as a move
        // above: forgotten, what the follow muted unmuted.
        for (auto it = fp.pending.begin(); it != fp.pending.end();) {
            if (!all.count(it->first)) {
                ++it;
                continue;
            }
            for (const std::string& g : it->second.muted) SetMute(all, g, false);
            arrange |= !it->second.muted.empty();
            it = fp.pending.erase(it);
        }
        // Still there, its record emptied: nothing to follow.
        for (auto it = fp.owners.begin(); it != fp.owners.end();)
            it = owners_now.count(it->first) ? std::next(it) : fp.owners.erase(it);
        // F9: an owner gone this tick, not moved. A cut (REAPER's last undo point names it) waits
        // for its paste, its populated sounds left behind muted meanwhile; any other deletion has
        // its sounds handled now.
        for (auto& kv : gone_now) {
            try {
                PendingGone& pg = kv.second;
                if (!IsCutUndoName(top_undo, CutNames())) {
                    arrange |= HandleGone(proj, pg.snap, all);
                    Remember(fp, pg.snap);
                    continue;
                }
                const std::vector<std::string> owners = OwnerGuids(pg.snap);
                for (const SoundPlacement& p : pg.snap.placed) {
                    if (p.own == Ownership::Attached || p.own == Ownership::Deleted) continue;
                    const auto it = all.find(Upper(p.item_guid));
                    if (it == all.end() || it->second.facts.muted || !it->second.facts.has_tag) continue;
                    bool ours = false;
                    for (const std::string& o : owners) ours |= SameFollowGuid(o, it->second.facts.tag_owner);
                    if (!ours) continue;
                    SetMute(all, p.item_guid, true);
                    pg.muted.push_back(p.item_guid);
                    arrange = true;
                }
                fp.pending[kv.first] = pg;
            } catch (...) {
            }
        }
        // A cut waits while it is REAPER's last undo point (or a selection change above it, fix C
        // review 2: REAPER's preference "include selection in undo"); another undo point (its paste
        // was matched above, or any other gesture): its sounds handled now.
        for (auto it = fp.pending.begin(); it != fp.pending.end();) {
            if (GestureUndoAvailable(top_undo, it->second.undo_desc, 0, -1) ||
                IsSelectionUndoName(top_undo, SelectionNames())) {
                ++it;
                continue;
            }
            try {
                arrange |= HandleGone(proj, it->second.snap, all);
                Remember(fp, it->second.snap);
            } catch (...) {
            }
            it = fp.pending.erase(it);
        }
        fp.scanned = true;
        // Detections of items that no longer have sounds (or are gone): forgotten.
        auto& det = g_detected[proj];
        for (auto it = det.begin(); it != det.end();) it = owners_now.count(it->first) ? std::next(it) : det.erase(it);

        if (arrange) UpdateArrange();
        if (!copies.empty()) Report(proj, PopulateItems(copies).lines);  // 13-8's own undo point
    } catch (...) {
    }
}

std::vector<ReportLine> FollowReport(ReaProject* proj)
{
    const auto it = g_report.find(proj ? proj : CurrentProject());
    if (it == g_report.end()) return {};
    return std::vector<ReportLine>(it->second.begin(), it->second.end());
}

void ClearFollowReport(ReaProject* proj)
{
    g_report.erase(proj ? proj : CurrentProject());
}

void DrawSoundsMenuSection()
{
    static ui::Section sec;
    if (ui::BeginSection(sec, "Sounds")) {
        ReaProject* proj = CurrentProject();
        bool        keep = LoadPrefFloat(kKeepDrawPref, 0.0f) != 0.0f;
        if (ui::Checkbox("Keep the draw on animation copies", &keep)) SavePrefFloat(kKeepDrawPref, keep ? 1.0f : 0.0f);
        ui::ItemTipText("On: a copied animation item plays the same variations.\nOff: it draws new ones.");
        DrawTracksRow(proj);  // 13-13

        int                           placed = 0;
        const std::vector<MediaItem*> items = RepopulateItems(proj, &placed);
        if (items.empty()) g_confirm_reset = false;
        if (g_confirm_reset) {
            ui::ConfirmBand band;
            ui::BeginConfirmBand(band);
            ImGui::Text("Remove %d placed sound%s and populate again?", placed, placed == 1 ? "" : "s");
            if (ui::ConfirmButton("Repopulate##sndreset")) {
                g_confirm_reset = false;
                Repopulate(proj);
            }
            ImGui::SameLine();
            if (ui::SolidButton("Cancel##sndresetcancel")) g_confirm_reset = false;
            ui::EndConfirmBand(band);
        } else {
            ImGui::BeginDisabled(items.empty());
            if (ui::SolidButton("Repopulate (reset)##sndreset", ImVec2(-1.0f, 0.0f))) g_confirm_reset = true;
            ImGui::EndDisabled();
            ui::ItemTipText(items.empty() ? "Select animation items that have sounds"
                                          : "Removes the populated sounds of the selected animation items,\n"
                                            "forgets the ones deleted by hand and draws again.\n"
                                            "Attached and spread sounds stay.");
        }

        const std::vector<ReportLine> rep = FollowReport(proj);
        if (rep.empty()) {
            ui::Caption("Follow report: nothing to report");
        } else {
            ImGui::Text("Follow report: %d line%s", static_cast<int>(rep.size()), rep.size() == 1 ? "" : "s");
            ImGui::SameLine();
            if (ui::SolidButton("Clear##sndfollowclear")) ClearFollowReport(proj);
            const float h = std::min(8, static_cast<int>(rep.size())) * ImGui::GetTextLineHeightWithSpacing() + ui::Dp(4.0f);
            if (ImGui::BeginChild("##sndfollowreport", ImVec2(-1.0f, h))) {
                for (auto it = rep.rbegin(); it != rep.rend(); ++it) {
                    if (it->t > 0.0)
                        ImGui::TextWrapped("%s: %s (%.3f s)", KindText(it->kind), it->text.c_str(), it->t);
                    else
                        ImGui::TextWrapped("%s: %s", KindText(it->kind), it->text.c_str());
                }
            }
            ImGui::EndChild();
        }
    }
    ui::EndSection(sec);
}

}  // namespace rav

#endif  // _WIN32
