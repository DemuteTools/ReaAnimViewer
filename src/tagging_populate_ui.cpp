// SPDX-License-Identifier: MIT
//
// See tagging_populate_ui.h. Story 13-8: the rule's Sound section, the item's sound values, the
// event's sound, the Populate row, the report line and the strip glyph. Every gesture is one
// REAPER undo point (sound_populate.h); the decisions are sound_populate_plan.h's.

#include "tagging_populate_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "library_icons.h"
#include "picked_gestures.h"
#include "reaper_api.h"
#include "rule_record.h"
#include "sound_assign.h"
#include "sound_auto_assign.h"
#include "sound_populate.h"
#include "sound_populate_plan.h"
#include "sound_project.h"
#include "sound_record.h"
#include "sound_switch.h"
#include "tagging_assign_ui.h"
#include "tagging_session.h"
#include "ui_flow.h"
#include "ui_flow_math.h"
#include "ui_theme.h"
#include "video_view.h"

namespace rav {

namespace {

const char kDot[] = "\xC2\xB7";  // "·"

// The shown item's sound record, read again when the project's state changes (P16).
struct RecCache {
    MediaItem*  item = nullptr;
    int         count = -1;
    ItemSounds  rec;
    std::string guid;
};
RecCache g_rec;

const RecCache& Rec(MediaItem* item)
{
    const int count = GetProjectStateChangeCount(nullptr);
    if (item != g_rec.item || count != g_rec.count) {
        g_rec.item = item;
        g_rec.count = count;
        g_rec.rec = item ? ReadItemSounds(item) : ItemSounds{};
        g_rec.guid = item ? ItemGuidOf(item) : std::string();
    }
    return g_rec;
}

// The footer's event count (P15): recomputed when the selection or the state changes, at most
// every 500 ms.
struct CountCache {
    std::vector<MediaItem*>               sel;
    int                                   count = -1;
    int                                   events = 0;
    bool                                  valid = false;
    std::chrono::steady_clock::time_point at;
};
CountCache g_count;

int FooterEvents(const std::vector<MediaItem*>& sel)
{
    const int  count = GetProjectStateChangeCount(nullptr);
    const auto now = std::chrono::steady_clock::now();
    const bool changed = !g_count.valid || sel != g_count.sel || count != g_count.count;
    if (changed && (!g_count.valid || now - g_count.at >= std::chrono::milliseconds(500))) {
        g_count.sel = sel;
        g_count.count = count;
        g_count.events = sel.empty() ? 0 : CountPopulateEvents(sel);
        g_count.valid = true;
        g_count.at = now;
    }
    return g_count.events;
}

// The last report (Populate's, or a gesture's that listed something).
struct LastReport {
    bool        has = false;
    SoundReport report;
    int         events = -1;  // -1: a gesture, not a Populate
    std::string summary;
    // Antho 2026-10-12: Populate keeps the sounds deleted by hand; the line's "Restore" places them again.
    int                     deleted_kept = 0;
    std::vector<MediaItem*> restore_items;
};
LastReport g_last;

void Remember(const SoundReport& rep, int events)
{
    if (events < 0 && rep.lines.empty()) return;  // a gesture that listed nothing keeps the last one
    g_last.has = true;
    g_last.report = rep;
    g_last.events = events;
    g_last.summary = ReportSummary(rep, events);
}

// Antho 2026-10-12 (13-8 fix): the menus below list the sound store by reference, and a gesture
// may replace that store (an auto block's value or library also goes to its marker type's store
// entry). Run from the menu, the gesture got its value through a reference into
// the store it replaced (the value written was whatever reused that memory: "it does not
// change"), and the menu went on reading the freed lists (a garbage length: REAPER hung). So a
// menu only picks: the gesture, holding its own copies, runs after the panel's widgets
// (RunPickedSoundGestures). Still one undo point each.
PickedGestures g_picks;

void Pick(std::function<SoundReport()> gesture)
{
    g_picks.Pick([gesture]() { Remember(gesture(), -1); });
}

std::string ValueName(const SwitchGroup* g, const std::string& uid)
{
    if (g)
        for (const SwitchValue& v : g->values)
            if (v.uid == uid) return v.name;
    return uid.empty() ? std::string("none") : std::string("?");
}

const char* FromWord(ValueFrom f)
{
    switch (f) {
    case ValueFrom::Item: return "item";
    case ValueFrom::Rule: return "rule";
    case ValueFrom::Event: return "event";
    case ValueFrom::Default: break;
    }
    return "default";
}

const ValueSet* FindValue(const std::vector<ValueSet>* vals, const std::string& group)
{
    if (!vals) return nullptr;
    for (const ValueSet& v : *vals)
        if (v.group_uid == group && !v.value_uid.empty()) return &v;
    return nullptr;
}

using FromTexts = std::vector<std::pair<std::string, std::string>>;

// One value menu (P14): "inherit · <value> (<from>)" then the group's values. `below` = the
// layers under this one; `own` = this level's values. `set` gets the value uid ("" = inherit).
// 13-15a: `texts` (optional) names where an inherited value comes from (group -> "Project · Feet").
void ValueMenu(const char* id, const SoundStore& store, const GroupUse& use, const ValueLayers& below,
               const std::vector<ValueSet>* own, const std::function<void(const std::string&)>& set,
               const FromTexts* texts = nullptr)
{
    const SwitchGroup* g = FindGroup(store, use.group_uid);
    if (!g || !use.first_switch) return;
    ValueFrom   from = ValueFrom::Default;
    std::string inh = EffectiveValue(*use.first_switch, below, &from);
    if (from == ValueFrom::Default) inh = DefaultValue(store, *use.first_switch);
    char inherit[192];
    std::string from_text = FromWord(from);
    if (texts && from != ValueFrom::Default)
        for (const auto& t : *texts)
            if (t.first == use.group_uid) from_text = t.second;
    std::snprintf(inherit, sizeof(inherit), "inherit %s %s (%s)", kDot, ValueName(g, inh).c_str(), from_text.c_str());
    const ValueSet* mine = FindValue(own, use.group_uid);
    const std::string preview = mine ? ValueName(g, mine->value_uid) : std::string(inherit);

    ImGui::PushID(id);
    // The group's name (user-typed) in the label column, muted as the mockup's .k.
    const float w = ui::LabelRowText(g->name, ui::Dp(120.0f), ui::kLabelCol, ui::kMuted);
    if (ui::BeginComboStyled("##val", preview.c_str(), w)) {
        if (ImGui::Selectable(inherit, mine == nullptr) && mine) set(std::string());
        for (const SwitchValue& v : g->values) {
            const bool cur = mine && mine->value_uid == v.uid;
            if (ImGui::Selectable((v.name + "##" + v.uid).c_str(), cur) && !cur) set(v.uid);
        }
        ui::EndComboStyled();
    }
    ImGui::PopID();
}

// The small arrow that opens the Library view (P14): library_icons.h's open arrow.
void OpenLibraryArrow()
{
    if (ui::IconOnlyButton("##openlib", ToolIcon(kToolOpenArrow), "Open the Library view")) SetViewMode(ViewMode::Library);
}

// The item's character (its rig, 13-15a), null when the Project's settings apply.
const SoundCharacter* CharacterOf(const SoundStore& store, MediaItem* item)
{
    const ItemRigInfo rig = ItemRig(item);
    return CharacterFor(store, rig.skeleton, rig.model);
}

const char* KindWord(ReportKind k)
{
    switch (k) {
    case ReportKind::Missing: return "Missing";
    case ReportKind::Replaced: return "Replaced";
    case ReportKind::Detached: return "Rule gone";
    case ReportKind::Blocked: return "Blocked";
    case ReportKind::Info: return "Note";
    case ReportKind::NoSound: return "No sound";
    }
    return "";
}

bool ListedMissing(const std::string& item_guid, double t)
{
    if (!g_last.has || item_guid.empty()) return false;
    for (const ReportLine& l : g_last.report.lines)
        if ((l.kind == ReportKind::Missing || l.kind == ReportKind::NoSound) && l.item_guid == item_guid && std::fabs(l.t - t) <= kEventMatchSeconds)
            return true;
    return false;
}

}  // namespace

void DrawRuleSoundSection(const TaggingSoundContext& ctx)
{
    if (!ctx.item || !ctx.rules || ctx.rule < 0 || static_cast<size_t>(ctx.rule) >= ctx.rules->blocks.size()) return;
    const SoundStore& store = GetSoundStore(nullptr);
    // 13-15b: one summary line (both sides of a marker pair) and Assign..., which opens the assignment window.
    const std::vector<int> open = RuleGroupBlocks(ctx.rules->blocks, ctx.rule);
    std::vector<Block>     shown;
    for (int i : open) shown.push_back(ctx.rules->blocks[static_cast<size_t>(i)]);
    ImGui::PushID("ravrulesound");
    ui::Section s;
    if (ui::BeginSection(s, "Sound")) {
        if (store.libraries.empty()) {
            ui::SubText("No sound library in this project yet: build one in the Library view.");
            OpenLibraryArrow();
        } else {
            DrawAssignSummaryRow("##rulesum", ctx.item, shown, open, "Sound:");
        }
    }
    ui::EndSection(s);
    ImGui::PopID();
}

void DrawAutoSoundRow(MediaItem* item, const ItemRules& rules, const std::vector<std::string>& keys)
{
    if (keys.empty() || !item) return;
    if (GetSoundStore(nullptr).libraries.empty()) return;
    // 13-15b: "<type> summary + Assign...". The blocks of these markers, else what a new one would get (its
    // inherited summary only: Assign... stays off until Detect made the block, a library picked on no rule would drop).
    std::vector<int>   open;
    std::vector<Block> shown;
    for (size_t i = 0; i < rules.blocks.size(); ++i) {
        const std::string k = AutoSoundKeyOf(rules.blocks[i]);
        if (!k.empty() && std::find(keys.begin(), keys.end(), k) != keys.end()) {
            open.push_back(static_cast<int>(i));
            shown.push_back(rules.blocks[i]);
        }
    }
    if (shown.empty())
        for (const std::string& k : keys) {
            AutoKind     kind;
            const size_t colon = k.find(':');
            if (colon != std::string::npos && AutoKindFromWord(k.substr(0, colon), &kind) && colon + 1 < k.size())
                shown.push_back(MakeAutoBlock(kind, k[colon + 1], kAutoDefaultSens, 0.0));
        }
    std::string id = "##autosum";
    for (const std::string& k : keys) id += k;
    DrawAssignSummaryRow(id.c_str(), item, shown, open, "Sound:");
}

void DrawEventSoundSection(const TaggingSoundContext& ctx, const ShownEvent& e)
{
    if (!ctx.item || !ctx.rules || ctx.rule < 0 || static_cast<size_t>(ctx.rule) >= ctx.rules->blocks.size()) return;
    if (e.kind != ShownKind::Detected && e.kind != ShownKind::User) return;
    const SoundStore&  store = GetSoundStore(nullptr);
    const ItemSounds&  rec = Rec(ctx.item).rec;
    const Block&       blk = ctx.rules->blocks[static_cast<size_t>(ctx.rule)];
    const std::string& rid = blk.rid;
    const SoundCharacter* ch = CharacterOf(store, ctx.item);
    const EffectiveAssign ea_rule = ResolveBlock(store, rec, ch, blk);  // the rule's layers, no event
    const SoundLibrary*   lib = !ea_rule.library.empty() ? FindLibrary(store, ea_rule.library) : nullptr;
    if (!lib) return;
    const EventSounds* ev = FindEventSounds(rec, rid, e.t);
    MediaItem*   item = ctx.item;
    const int    block = ctx.rule;
    const double t = ev ? ev->t : e.t;

    ImGui::PushID("raveventsound");
    ui::Section s;
    if (ui::BeginSection(s, "Event sound")) {
        // The path it plays (its stored draws; nothing is drawn nor written here).
        PopulateDrawState st;
        st.lib = lib;
        st.seed = rec.seed ? rec.seed : 1;
        EventDraw ed;
        ed.rid = rid;
        ed.t = t;
        ed.prev = ev ? &ev->draws : nullptr;
        std::string part, type;
        PartOfBlock(blk, &part, &type);
        const Resolution res = ResolveEvent(store, *lib, ResolveAssignFor(store, rec, ch, rid, part, type, t).layers,
                                            MakeDrawHooks(st, ed, nullptr));
        ui::SubText(res.blocked ? ("Blocked: " + res.reason).c_str() : res.path_text.c_str());
        if (!res.blocked && !res.gap.empty()) ui::ToneText("Nothing placed: " + res.gap, ui::kWarn, true);
        bool undrawn = false;  // a pick not stored yet: Populate draws it (with the item's history)
        for (const SoundDraw& d : ed.out) {
            bool stored = false;
            if (ev)
                for (const SoundDraw& x : ev->draws)
                    if (x.node == d.node) stored = true;
            if (!stored) undrawn = true;
        }
        if (undrawn && !res.blocked) ui::Caption("Not drawn yet: Populate picks the random parts");
        int placed = 0, deleted = 0, attached = 0;
        if (ev)
            for (const SoundPlacement& p : ev->placed) {
                if (p.own == Ownership::Populated) ++placed;
                else if (p.own == Ownership::Deleted) ++deleted;
                else ++attached;
            }
        if (placed + deleted + attached > 0) {
            char line[128];
            std::snprintf(line, sizeof(line), "Placed %d %s deleted by hand %d %s attached %d", placed, kDot, deleted,
                          kDot, attached);
            ui::Caption(line);
        }
        ValueLayers below;
        below.item = ea_rule.layers.item;
        below.rule = ea_rule.layers.rule;
        for (const GroupUse& g : GroupsUsed(store, *lib)) {
            const std::string gid = g.group_uid;
            ValueMenu(gid.c_str(), store, g, below, ev ? &ev->overrides : nullptr,
                      [item, block, t, gid](const std::string& v) {
                          Pick([item, block, t, gid, v]() { return SetSoundValue(item, SoundValueLevel::Event, block, t, gid, v); });
                      },
                      &ea_rule.value_from);
        }
    }
    ui::EndSection(s);
    ImGui::PopID();
}

void RunPickedSoundGestures()
{
    g_picks.Run();
}

void PickSoundGesture(std::function<SoundReport()> gesture)
{
    Pick(std::move(gesture));
}

// Populate (or, with `restore`, Populate placing again the sounds deleted by hand: the line's Restore), then
// the line says what happened (Antho 2026-10-12: a Populate with nothing to do looked broken).
static void RunPopulate(const std::vector<MediaItem*>& items, bool restore, const PopulateProgressFn& progress)
{
    int               events = 0;
    PopulateCounts    counts;
    const SoundReport rep = PopulateItemsCounted(items, &events, &counts, restore, progress);
    Remember(rep, events);
    bool trouble = false;  // missing / blocked / no-sound lines: the summary already says why
    for (const ReportLine& l : rep.lines)
        if (l.kind == ReportKind::Missing || l.kind == ReportKind::Blocked || l.kind == ReportKind::NoSound)
            trouble = true;
    char buf[160];
    buf[0] = '\0';
    if (counts.placed + counts.replaced + counts.removed > 0)
        std::snprintf(buf, sizeof(buf), " \xC2\xB7 %d placed", counts.placed + counts.replaced);
    else if (counts.kept > 0 && !trouble && counts.deleted_kept == 0)
        std::snprintf(buf, sizeof(buf), " \xC2\xB7 already in place");
    else if (!trouble && counts.deleted_kept == 0)
        std::snprintf(buf, sizeof(buf), " \xC2\xB7 nothing placed");
    g_last.summary += buf;
    if (counts.deleted_kept > 0) {
        std::snprintf(buf, sizeof(buf), " \xC2\xB7 %d deleted by hand kept", counts.deleted_kept);
        g_last.summary += buf;
    }
    g_last.deleted_kept = counts.deleted_kept;
    g_last.restore_items = counts.deleted_kept > 0 ? items : std::vector<MediaItem*>();
    g_count.valid = false;
}

// 13-19: a Populate / Restore clicked this frame runs from RAV's window procedure, outside the ImGui
// frame, so RAV's window can show its progress (PopulateHasPending / RunPendingPopulate).
namespace {
struct PendingPopulate {
    bool                    has = false;
    bool                    restore = false;
    std::vector<MediaItem*> items;
};
PendingPopulate g_pending;
}  // namespace

static void QueuePopulate(const std::vector<MediaItem*>& items, bool restore)
{
    if (g_pending.has || items.empty()) return;
    g_pending.has = true;
    g_pending.restore = restore;
    g_pending.items = items;
}

static bool g_job_running = false;  // 13-19 review: the button stays disabled while the job runs

bool PopulateHasPending()
{
    return g_pending.has || g_job_running;
}

void RunPendingPopulate(const PopulateProgressFn& progress)
{
    if (!g_pending.has) return;
    const PendingPopulate job = g_pending;
    g_pending = PendingPopulate();
    std::vector<MediaItem*> items;  // the items clicked on, still in the project
    for (MediaItem* it : job.items)
        if (ValidatePtr2(nullptr, it, "MediaItem*")) items.push_back(it);
    if (items.empty()) return;
    struct Running {
        Running() { g_job_running = true; }
        ~Running() { g_job_running = false; }
    } running;
    RunPopulate(items, job.restore, progress);
}

void DropPendingPopulate()
{
    g_pending = PendingPopulate();
}

bool SoundFooterVisible(const TaggingSoundContext& ctx)
{
    return ctx.item != nullptr && !GetSoundStore(nullptr).libraries.empty();
}

float SoundFooterHeight(const TaggingSoundContext& ctx)
{
    return SoundFooterVisible(ctx) ? ImGui::GetFrameHeightWithSpacing() * 2.0f : 0.0f;
}

void DrawPopulateRow(const TaggingSoundContext& ctx, float attach_w)
{
    if (!SoundFooterVisible(ctx)) return;
    const std::vector<MediaItem*> sel = SelectedRavItems();
    const int   n = FooterEvents(sel);
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float reshuffle_w = ui::ButtonWidth("Reshuffle") + ui::Dp(8.0f);
    float avail = ImGui::GetContentRegionAvail().x - (attach_w > 0.0f ? attach_w + sp : 0.0f);
    const float populate_w = std::max(ui::Dp(60.0f), avail - reshuffle_w - sp);
    char label[64];
    if (g_pending.has)
        std::snprintf(label, sizeof(label), "Populating...##ravpopulate");
    else
        std::snprintf(label, sizeof(label), "Populate %s %d event%s##ravpopulate", kDot, n, n == 1 ? "" : "s");
    const bool none = sel.empty() || g_pending.has;
    if (none) ImGui::BeginDisabled();
    if (ui::PrimaryButton(label, ImVec2(populate_w, 0.0f))) QueuePopulate(sel, false);
    if (none) ImGui::EndDisabled();
    ui::ItemTipText(sel.empty() ? "Select animation items"
                         : "Places each event's sound on the selected items and their linked copies.\n"
                           "Placed sounds you moved stay where you put them; the ones you deleted by hand stay\n"
                           "deleted (Restore, on the line below, places them again). One undo point.");
    ImGui::SameLine();
    if (none) ImGui::BeginDisabled();
    if (ui::SolidButton("Reshuffle##ravreshuffle", ImVec2(reshuffle_w, 0.0f))) Remember(ReshuffleItems(sel), -1);
    if (none) ImGui::EndDisabled();
    ui::ItemTipText(sel.empty() ? "Select animation items"
                         : "New random picks for the placed sounds (hand picks kept). One undo point.");
    if (attach_w > 0.0f) ImGui::SameLine();
}

void DrawSoundReportLine(const TaggingSoundContext& ctx)
{
    if (!SoundFooterVisible(ctx)) return;
    const float help_w = ui::Dp(18.0f);  // ui::HelpMark's circle
    ImGui::AlignTextToFramePadding();
    if (!g_last.has) {
        ui::ToneText("Not populated yet", ui::kFaint);
    } else {
        // A quiet link (muted, underlined under the mouse), ellipsized to the room before Restore and the "?".
        const char* restore = "Restore##ravrestore";
        const bool  can_restore = g_last.deleted_kept > 0 && !g_last.restore_items.empty();
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float restore_w = can_restore ? ImGui::CalcTextSize("Restore").x + sp : 0.0f;
        const float w = std::max(ui::Dp(20.0f), ImGui::GetContentRegionAvail().x - help_w - sp - restore_w);
        const flow::MeasureFn measure = [](const char* b, const char* e) { return ImGui::CalcTextSize(b, e).x; };
        bool              cut = false;
        const std::string shown = flow::Ellipsize(g_last.summary, w, "...", measure, &cut);
        if (ui::LinkButton((shown + "##ravreport").c_str(), ui::kMuted)) ImGui::OpenPopup("##ravreportpop");
        ui::ItemTipText(cut ? (g_last.summary + "\nClick for the details").c_str() : "Click for the details");
        if (can_restore) {
            ImGui::SameLine();
            if (ui::LinkButton(restore)) QueuePopulate(g_last.restore_items, true);  // 13-19: checked when it runs
            ui::ItemTipText("Places again the sounds you deleted by hand on these items. One undo point.");
        }
    }
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - help_w));
    ui::HelpMark("##ravglyphs");
    if (ui::BeginItemTip()) {
        // The mock-up's legend: each mark drawn as on the strip, then what it means.
        ImGui::TextUnformatted("Under each event on the strip:");
        static const struct { int glyph; const char* text; } kLegend[] = {
            {kGlyphEvPopulated, "sound placed by Populate"},
            {kGlyphEvAttached, "your attached sound"},
            {kGlyphEvSpread, "Spread copy"},
            {kGlyphEvMissing, "sound missing in the last Populate"},
            {kGlyphEvDeleted, "deleted by hand (not put back)"},
        };
        const float lh = ImGui::GetTextLineHeight();
        for (const auto& l : kLegend) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            DrawEventGlyph(ImGui::GetWindowDrawList(), ImVec2(p.x + lh * 0.5f, p.y + lh * 0.5f), ui::Dp(9.0f), l.glyph);
            ImGui::Dummy(ImVec2(lh, lh));
            ImGui::SameLine();
            ImGui::TextUnformatted(l.text);
        }
        ui::EndItemTip();
    }
    if (ui::BeginPanelPopup("##ravreportpop")) {
        if (g_last.report.lines.empty()) ImGui::TextUnformatted("Nothing to report.");
        for (size_t i = 0; i < g_last.report.lines.size(); ++i) {
            const ReportLine& l = g_last.report.lines[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::AlignTextToFramePadding();
            ui::ToneText(KindWord(l.kind), l.kind == ReportKind::Replaced || l.kind == ReportKind::Info ? ui::kMuted : ui::kWarn);
            ImGui::SameLine();
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::SameLine();
            if (ui::LinkButton("Open")) SetViewMode(ViewMode::Library);
            ui::ItemTipText("Open the Library view");
            ImGui::SameLine();
            if (ui::LinkButton("Select")) SelectOnlyItem(FindItemByGuid(l.item_guid));
            ui::ItemTipText("Select the animation item in REAPER");
            ImGui::PopID();
        }
        ui::EndPanelPopup();
    }
}

void DrawSoundStripGlyph(ImDrawList* dl, const TaggingModel& m, const ShownEvent& e, float x, float y)
{
    if (!dl || !m.item) return;
    if (e.kind != ShownKind::Detected && e.kind != ShownKind::User) return;
    if (e.block < 0 || static_cast<size_t>(e.block) >= m.rules.blocks.size()) return;
    const std::string& rid = m.rules.blocks[static_cast<size_t>(e.block)].rid;
    if (rid.empty()) return;
    const RecCache& rc = Rec(m.item);
    const EventSounds* ev = FindEventSounds(rc.rec, rid, e.t);
    const bool missing = ListedMissing(rc.guid, e.t);
    if (!ev && !missing) return;
    bool populated = false, attached = false, spread = false, deleted = false;
    if (ev)
        for (const SoundPlacement& p : ev->placed) {
            if (p.own == Ownership::Populated) populated = true;
            else if (p.own == Ownership::Attached) attached = true;
            else if (p.own == Ownership::Spread) spread = true;
            else deleted = true;
        }
    // The mock-up's glyph(): a 9 px mark under the event (library_icons.h kGlyphEv*).
    const ImVec2 c(x, y - ui::Dp(5.0f));
    const float  s = ui::Dp(9.0f);
    if (missing) DrawEventGlyph(dl, c, s, kGlyphEvMissing);
    else if (attached) DrawEventGlyph(dl, c, s, kGlyphEvAttached);
    else if (spread) DrawEventGlyph(dl, c, s, kGlyphEvSpread);
    else if (populated) DrawEventGlyph(dl, c, s, kGlyphEvPopulated);
    else if (deleted) DrawEventGlyph(dl, c, s, kGlyphEvDeleted);
}

}  // namespace rav

#endif  // _WIN32
