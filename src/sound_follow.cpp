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
#include <map>
#include <set>
#include <string>
#include <vector>

#include "event_list.h"
#include "imgui.h"
#include "item_rules.h"
#include "shortcuts.h"
#include "sound_follow_plan.h"
#include "sound_place.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "sound_record.h"
#include "tagging_signal.h"
#include "ui_theme.h"

namespace rav {

namespace {

using SetMediaItemInfo_ValueFn = bool (*)(MediaItem* item, const char* parmname, double newvalue);
SetMediaItemInfo_ValueFn f_set_item = nullptr;

constexpr size_t kReportMax = 200;
constexpr char   kKeepDrawPref[] = "sound_copy_keeps_draw";

struct FollowProject {
    bool                                               scanned = false;
    std::set<std::string>                              seen;    // item GUIDs (upper case)
    std::map<std::string, std::vector<SoundPlacement>> owners;  // owner GUID -> its placements
};
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

std::string ItemGuidOf(MediaItem* item)
{
    char buf[64] = {};
    if (!item || !GetSetMediaItemInfo_String(item, "GUID", buf, false)) return {};
    return buf;
}

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

// The item's clip map from REAPER alone (the twin of tag_markers.cpp's file-local MirrorClipMap).
bool FollowClipMap(MediaItem* item, MediaItem_Take* take, ItemClipMap* out)
{
    PCM_source* src = GetMediaItemTake_Source(take);
    for (int depth = 0; src && depth < 4; ++depth) {
        const char* type = src->GetType();
        if (type && std::strcmp(type, "RAV_ANIM") == 0) break;
        src = src->GetSource();
    }
    if (!src) return false;
    bool         qn = false;
    const double len = GetMediaSourceLength(src, &qn);
    if (qn || !(len > 0.0) || !std::isfinite(len)) return false;
    ItemClipMap map;
    map.clip_len = len;
    map.item_pos = GetMediaItemInfo_Value(item, "D_POSITION");
    map.item_len = GetMediaItemInfo_Value(item, "D_LENGTH");
    map.loop = GetMediaItemInfo_Value(item, "B_LOOPSRC") != 0.0;
    map.start_offs = GetMediaItemTakeInfo_Value(take, "D_STARTOFFS");
    const double rate = GetMediaItemTakeInfo_Value(take, "D_PLAYRATE");
    map.rate = (rate > 0.0 && std::isfinite(rate)) ? rate : 1.0;
    *out = map;
    return true;
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

// The amber in-place confirm's action button (tagging_view_ui.cpp's pattern).
bool AmberButton(const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ui::Col(ui::kConfirmAct));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::Col(ui::kConfirmActHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::Col(ui::kConfirmAct));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kConfirmActText));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return pressed;
}

const char* KindText(ReportKind k)
{
    switch (k) {
    case ReportKind::Missing: return "Missing";
    case ReportKind::Replaced: return "Replaced";
    case ReportKind::Detached: return "Detached";
    case ReportKind::Blocked: return "Blocked";
    }
    return "";
}

bool g_confirm_reset = false;

}  // namespace

void InitSoundFollow(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_set_item = reinterpret_cast<SetMediaItemInfo_ValueFn>(get_func("SetMediaItemInfo_Value"));
}

void SoundFollowTick(ReaProject* proj)
{
    try {
        if (!proj) return;
        FollowProject& fp = g_follow[proj];

        // One pass over the items: the facts of every item.
        std::map<std::string, ReadItem> all;
        std::vector<MediaItem*>         anims;
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
            PlacedTag tag;
            if (ReadPlacedTag(item, &tag)) {
                f.has_tag = true;
                f.tag_owner = tag.owner;
                f.tag_t = tag.t;
            }
            f.has_store = ParseFollowStore(ItemExt(item, kFollowStoreKey), &f.store);
            if (RavTakeOf(item)) anims.push_back(item);
            all[Upper(f.item_guid)] = r;
        }

        const bool               keep_draw = LoadPrefFloat(kKeepDrawPref, 0.0f) != 0.0f;
        const SoundStore&        store = GetSoundStore(proj);
        std::vector<std::pair<std::string, std::string>> libs;
        for (const SoundLibrary& l : store.libraries) libs.emplace_back(l.uid, l.name);

        bool                    arrange = false;
        std::vector<MediaItem*> copies;
        std::set<std::string>   owners_now;
        for (MediaItem* item : anims) {
            try {
                const std::string guid = ItemGuidOf(item);
                const std::string key = Upper(guid);
                const bool        appeared = fp.scanned && fp.seen.count(key) == 0;
                fp.seen.insert(key);
                const ItemSounds rec = ReadItemSounds(item);
                if (!HasSounds(rec)) continue;
                owners_now.insert(key);
                ItemRulesRead rr;
                if (!ReadItemRules(item, &rr) || !rr.valid) {
                    fp.owners[key] = AllPlacements(rec);
                    continue;  // unreadable rules: nothing is decided
                }
                ItemClipMap map;
                if (!FollowClipMap(item, RavTakeOf(item), &map)) {
                    fp.owners[key] = AllPlacements(rec);
                    continue;
                }
                FollowInput in;
                in.owner_guid = guid;
                in.rec = rec;
                in.events = CommittedFollowEvents(rr.rules, map);
                for (const auto& b : rr.rules.blocks)
                    if (!b.rid.empty()) in.rule_rids.push_back(b.rid);
                in.items = FactsFor(all, AllPlacements(rec));
                in.library_names = libs;
                in.appeared = appeared;
                in.keep_draw = keep_draw;
                in.copy_seed = FollowGuidHash(guid);
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
                fp.owners[key] = AllPlacements(plan.record);
                if (plan.populate_copy) copies.push_back(item);
            } catch (...) {
            }
        }

        // F9: owners gone since the last scan.
        for (auto it = fp.owners.begin(); it != fp.owners.end();) {
            if (owners_now.count(it->first)) {
                ++it;
                continue;
            }
            if (all.count(it->first)) {  // still there, its record emptied: nothing to follow
                it = fp.owners.erase(it);
                continue;
            }
            try {
                const FollowPlan plan = PlanOwnerGone(it->first, it->second, FactsFor(all, it->second));
                arrange |= Apply(proj, plan, all);
                Report(proj, plan.report);
            } catch (...) {
            }
            it = fp.owners.erase(it);
        }
        fp.scanned = true;

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
        if (ImGui::Checkbox("Keep the draw on animation copies", &keep)) SavePrefFloat(kKeepDrawPref, keep ? 1.0f : 0.0f);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("On: a copied animation item plays the same variations.\nOff: it draws new ones.");

        int                           placed = 0;
        const std::vector<MediaItem*> items = RepopulateItems(proj, &placed);
        if (items.empty()) g_confirm_reset = false;
        if (g_confirm_reset) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float w = ImGui::GetContentRegionAvail().x;
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            const ImVec2 b0 = ImGui::GetCursorScreenPos();
            ImGui::Indent(6.0f);
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::Text("Remove %d placed sound%s and populate again?", placed, placed == 1 ? "" : "s");
            if (AmberButton("Repopulate##sndreset")) {
                g_confirm_reset = false;
                Repopulate(proj);
            }
            ImGui::SameLine();
            if (ui::SolidButton("Cancel##sndresetcancel")) g_confirm_reset = false;
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
            ImGui::Unindent(6.0f);
            const ImVec2 b1(b0.x + w, ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y * 0.5f);
            dl->ChannelsSetCurrent(0);
            dl->AddRectFilled(b0, b1, (ui::kConfirmLine & ~IM_COL32_A_MASK) | (0x14u << IM_COL32_A_SHIFT), ui::kRadiusMd);
            dl->AddRect(b0, b1, ui::kConfirmLine, ui::kRadiusMd);
            dl->ChannelsMerge();
        } else {
            ImGui::BeginDisabled(items.empty());
            if (ui::SolidButton("Repopulate (reset)##sndreset", ImVec2(-1.0f, 0.0f))) g_confirm_reset = true;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(items.empty()
                                      ? "Select animation items that have sounds"
                                      : "Removes the populated sounds of the selected animation items,\n"
                                        "forgets the ones deleted by hand and draws again.\n"
                                        "Attached and spread sounds stay.");
        }

        const std::vector<ReportLine> rep = FollowReport(proj);
        if (rep.empty()) {
            ImGui::TextDisabled("Follow report: nothing to report");
        } else {
            ImGui::Text("Follow report: %d line%s", static_cast<int>(rep.size()), rep.size() == 1 ? "" : "s");
            ImGui::SameLine();
            if (ui::SolidButton("Clear##sndfollowclear")) ClearFollowReport(proj);
            const float h = std::min(8, static_cast<int>(rep.size())) * ImGui::GetTextLineHeightWithSpacing() + 4.0f;
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
