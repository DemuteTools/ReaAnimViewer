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
    return CharacterFor(store, rig.skeleton, rig.model_file);
}

const ItemSounds kNoSounds;

std::string LibraryNameOf(const SoundStore& store, const std::string& uid)
{
    const SoundLibrary* lib = FindLibrary(store, uid);
    return lib ? lib->name : std::string("(gone)");
}

// "inherit · Wood steps (Project · Feet · Step)" / "inherit · None (Hero · Feet)" / "inherit · nothing set".
std::string InheritText(const SoundStore& store, const EffectiveAssign& inh)
{
    if (inh.level == AssignLevel::Unset) return std::string("inherit ") + kDot + " nothing set";
    const std::string what = inh.none ? std::string("None") : LibraryNameOf(store, inh.library);
    return std::string("inherit ") + kDot + " " + what + " (" + inh.from + ")";
}

// The libraries the item's rules play (their resolved library), each once.
std::vector<const SoundLibrary*> ItemLibraries(const SoundStore& store, const ItemSounds& rec, const ItemRules& rules,
                                               const SoundCharacter* ch)
{
    std::vector<const SoundLibrary*> out;
    for (const Block& b : rules.blocks) {
        if (b.rid.empty()) continue;
        const EffectiveAssign ea = ResolveBlock(store, rec, ch, b);
        if (ea.library.empty()) continue;
        const SoundLibrary* lib = FindLibrary(store, ea.library);
        bool have = false;
        for (const SoundLibrary* l : out)
            if (l == lib) have = true;
        if (lib && !have) out.push_back(lib);
    }
    return out;
}

const char* KindWord(ReportKind k)
{
    switch (k) {
    case ReportKind::Missing: return "Missing";
    case ReportKind::Replaced: return "Replaced";
    case ReportKind::Detached: return "Rule gone";
    case ReportKind::Blocked: return "Blocked";
    case ReportKind::Info: return "Note";
    }
    return "";
}

bool ListedMissing(const std::string& item_guid, double t)
{
    if (!g_last.has || item_guid.empty()) return false;
    for (const ReportLine& l : g_last.report.lines)
        if (l.kind == ReportKind::Missing && l.item_guid == item_guid && std::fabs(l.t - t) <= kEventMatchSeconds)
            return true;
    return false;
}

}  // namespace

void DrawRuleSoundSection(const TaggingSoundContext& ctx)
{
    if (!ctx.item || !ctx.rules || ctx.rule < 0 || static_cast<size_t>(ctx.rule) >= ctx.rules->blocks.size()) return;
    const SoundStore&  store = GetSoundStore(nullptr);
    const ItemSounds&  rec = Rec(ctx.item).rec;
    const Block&       blk = ctx.rules->blocks[static_cast<size_t>(ctx.rule)];
    const std::string& rid = blk.rid;
    const RuleSounds*  rs = rid.empty() ? nullptr : FindRuleSounds(rec, rid);
    // 13-15a: the library and values this rule plays come from its layers (rule, Character, Project).
    const SoundCharacter* ch = CharacterOf(store, ctx.item);
    const EffectiveAssign ea = ResolveBlock(store, rec, ch, blk);
    std::string           part, type;
    PartOfBlock(blk, &part, &type);
    const EffectiveAssign inh = ResolveAssign(store, kNoSounds, ch, std::string(), part, type);  // with no rule of its own
    const SoundLibrary*   lib = !ea.library.empty() ? FindLibrary(store, ea.library) : nullptr;
    const bool            own_none = rs && rs->none;
    const bool            own_lib = rs && !rs->library.empty();
    MediaItem* item = ctx.item;
    const int  block = ctx.rule;

    ImGui::PushID("ravrulesound");
    ui::Section s;
    if (ui::BeginSection(s, "Sound")) {
        if (store.libraries.empty()) {
            ui::SubText("No sound library in this project yet: build one in the Library view.");
            OpenLibraryArrow();
        } else {
            // The combo leaves room for the open-library arrow after it.
            const float arrow_w = ui::ControlH() + ImGui::GetStyle().ItemSpacing.x;
            const float field_w = ui::LabelRow("Library", ui::Dp(60.0f) + arrow_w);
            const std::string preview = own_none  ? std::string("None (place nothing)")
                                        : own_lib ? LibraryNameOf(store, rs->library)
                                                  : InheritText(store, inh);
            if (ui::BeginComboStyled("##lib", preview.c_str(), std::max(ui::Dp(60.0f), field_w - arrow_w))) {
                const bool inherits = !own_none && !own_lib;
                if (ImGui::Selectable((InheritText(store, inh) + "##inherit").c_str(), inherits) && !inherits)
                    Pick([item, block]() { return SetRuleLibrary(item, block, std::string()); });
                if (ImGui::Selectable("None (place nothing)", own_none) && !own_none)
                    Pick([item, block]() { return SetRuleLibrary(item, block, kLibNoneWord); });
                for (const SoundLibrary& L : store.libraries) {
                    const LibraryCheck chk = CheckLibrary(store, L);
                    if (chk.blocked) ImGui::BeginDisabled();
                    const bool cur = own_lib && rs->library == L.uid;
                    if (ImGui::Selectable((L.name + "##" + L.uid).c_str(), cur) && !cur && !chk.blocked)
                        Pick([item, block, uid = L.uid]() { return SetRuleLibrary(item, block, uid); });
                    if (chk.blocked) ImGui::EndDisabled();
                    if (chk.blocked) ui::ItemTip("Blocked: %s", chk.reason.c_str());
                }
                ui::EndComboStyled();
            }
            ImGui::SameLine();
            OpenLibraryArrow();
            if (lib) {
                for (const GroupUse& g : GroupsUsed(store, *lib)) {
                    const std::string gid = g.group_uid;
                    ValueMenu(gid.c_str(), store, g, ValueLayers{ea.layers.item, {}, {}}, rs ? &rs->values : nullptr,
                              [item, block, gid](const std::string& v) {
                                  Pick([item, block, gid, v]() { return SetSoundValue(item, SoundValueLevel::Rule, block, 0.0, gid, v); });
                              },
                              &ea.item_from);
                }
            }
        }
    }
    ui::EndSection(s);
    ImGui::PopID();
}

void DrawAutoSoundRow(MediaItem* item, const ItemRules& rules, const std::vector<std::string>& keys)
{
    if (keys.empty()) return;
    const SoundStore& store = GetSoundStore(nullptr);
    if (store.libraries.empty()) return;
    const ItemSounds&     rec = Rec(item).rec;
    const SoundCharacter* ch = CharacterOf(store, item);
    std::vector<AutoSoundShown> shown;
    for (const std::string& k : keys) shown.push_back(AutoSoundFor(store, rec, ch, rules.blocks, k));
    bool same_own = true, same_eff = true, on_item = false;
    for (const AutoSoundShown& s : shown) {
        if (s.own_state != shown[0].own_state || s.own_library != shown[0].own_library) same_own = false;
        if (s.effective.library != shown[0].effective.library || s.effective.none != shown[0].effective.none) same_eff = false;
        if (s.on_item) on_item = true;
    }
    const AutoSoundShown& first = shown[0];
    const SoundLibrary*   lib = same_eff && !first.effective.library.empty() ? FindLibrary(store, first.effective.library) : nullptr;
    const std::string     inherit_text = same_eff ? InheritText(store, first.effective) : std::string("inherit");
    const std::string     preview = !same_own ? std::string("Mixed")
                                    : first.own_state == LibState::None    ? std::string("None (place nothing)")
                                    : first.own_state == LibState::Library ? LibraryNameOf(store, first.own_library)
                                                                           : inherit_text;
    std::string         id;
    for (const std::string& k : keys) id += k + ",";
    const std::vector<std::string> keys_copy = keys;

    ImGui::PushID(id.c_str());
    ImGui::AlignTextToFramePadding();
    ui::ToneText("Sound", ui::kMuted);
    ui::SameLineIfFits(ui::MenuButtonWidth(preview.c_str()));
    if (ui::MenuButton("##autolib", true, preview.c_str())) ImGui::OpenPopup("##autolibmenu");
    ui::ItemTipText(on_item ? "The library of these markers: the same as their rule's Sound card.\n"
                              "Inherit follows the Character, then the Project."
                            : "No marker of this type on the item yet.\nInherit follows the Character, then the Project.");
    if (ui::BeginMenuPopup("##autolibmenu")) {
        const bool inherits = same_own && first.own_state == LibState::Inherit;
        if (ImGui::Selectable((inherit_text + "##inherit").c_str(), inherits) && !inherits)
            Pick([item, keys_copy]() { return SetAutoMarkerLibrary(item, keys_copy, std::string()); });
        const bool is_none = same_own && first.own_state == LibState::None;
        if (ImGui::Selectable("None (place nothing)", is_none) && !is_none)
            Pick([item, keys_copy]() { return SetAutoMarkerLibrary(item, keys_copy, kLibNoneWord); });
        for (const SoundLibrary& L : store.libraries) {
            const LibraryCheck chk = CheckLibrary(store, L);
            if (chk.blocked) ImGui::BeginDisabled();
            const bool cur = same_own && first.own_state == LibState::Library && first.own_library == L.uid;
            if (ImGui::Selectable((L.name + "##" + L.uid).c_str(), cur) && !cur && !chk.blocked)
                Pick([item, keys_copy, uid = L.uid]() { return SetAutoMarkerLibrary(item, keys_copy, uid); });
            if (chk.blocked) ImGui::EndDisabled();
            if (chk.blocked) ui::ItemTip("Blocked: %s", chk.reason.c_str());
        }
        ui::EndMenuPopup();
    }
    if (lib) {
        // One compact menu per Switch Group the library uses: "<group>: <value>" ("inherit" = the
        // item's value, the Character's / Project's or the Switch's default, as the rule's Sound card says).
        ValueLayers below;
        below.item = first.effective.layers.item;
        for (const GroupUse& g : GroupsUsed(store, *lib)) {
            const SwitchGroup* grp = FindGroup(store, g.group_uid);
            if (!grp || !g.first_switch) continue;
            std::string val;
            bool        same_val = true;
            for (size_t i = 0; i < shown.size(); ++i) {
                const ValueSet* v = FindValue(&shown[i].values, g.group_uid);
                const std::string uid = v ? v->value_uid : std::string();
                if (i == 0) val = uid;
                else if (uid != val) same_val = false;
            }
            const std::string label =
                grp->name + ": " + (!same_val ? std::string("Mixed") : val.empty() ? std::string("inherit") : ValueName(grp, val));
            ImGui::PushID(g.group_uid.c_str());
            ui::SameLineIfFits(ui::MenuButtonWidth(label.c_str()));
            if (ui::MenuButton("##autoval", true, label.c_str())) ImGui::OpenPopup("##autovalmenu");
            if (ui::BeginMenuPopup("##autovalmenu")) {
                ValueFrom   from = ValueFrom::Default;
                std::string inh = EffectiveValue(*g.first_switch, below, &from);
                if (from == ValueFrom::Default) inh = DefaultValue(store, *g.first_switch);
                std::string from_text = FromWord(from);
                if (from != ValueFrom::Default)
                    for (const auto& t : first.effective.item_from)
                        if (t.first == g.group_uid) from_text = t.second;
                char inherit[192];
                std::snprintf(inherit, sizeof(inherit), "inherit %s %s (%s)", kDot, ValueName(grp, inh).c_str(), from_text.c_str());
                const std::string gid = g.group_uid;
                const bool        own = !same_val || !val.empty();
                if (ImGui::Selectable(inherit, !own) && own)
                    Pick([item, keys_copy, gid]() { return SetAutoMarkerValue(item, keys_copy, gid, std::string()); });
                for (const SwitchValue& v : grp->values) {
                    const bool cur = same_val && val == v.uid;
                    if (ImGui::Selectable((v.name + "##" + v.uid).c_str(), cur) && !cur)
                        Pick([item, keys_copy, gid, uid = v.uid]() { return SetAutoMarkerValue(item, keys_copy, gid, uid); });
                }
                ui::EndMenuPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::PopID();
}

void DrawItemSoundValuesSection(const TaggingSoundContext& ctx)
{
    if (!ctx.item || !ctx.rules) return;
    const SoundStore& store = GetSoundStore(nullptr);
    if (store.libraries.empty()) return;
    const ItemSounds& rec = Rec(ctx.item).rec;
    std::vector<GroupUse> groups;
    for (const SoundLibrary* lib : ItemLibraries(store, rec, *ctx.rules, CharacterOf(store, ctx.item)))
        for (const GroupUse& g : GroupsUsed(store, *lib)) {
            bool have = false;
            for (const GroupUse& x : groups)
                if (x.group_uid == g.group_uid) have = true;
            if (!have) groups.push_back(g);
        }
    if (groups.empty()) return;
    MediaItem* item = ctx.item;
    ImGui::PushID("ravitemsound");
    const char* title = "Item sound values";
    {  // P14: starts closed (BeginSection keeps "##open" under the title's ID).
        ImGui::PushID(title);
        ImGuiStorage* st = ImGui::GetStateStorage();
        const ImGuiID id = ImGui::GetID("##open");
        if (st->GetInt(id, -1) == -1) st->SetBool(id, false);
        ImGui::PopID();
    }
    ui::Section s;
    if (ui::BeginSection(s, title)) {
        for (const GroupUse& g : groups) {
            const std::string gid = g.group_uid;
            ValueMenu(gid.c_str(), store, g, ValueLayers{}, &rec.item_values, [item, gid](const std::string& v) {
                Pick([item, gid, v]() { return SetSoundValue(item, SoundValueLevel::Item, -1, 0.0, gid, v); });
            });
        }
    }
    ui::EndSection(s);
    ImGui::PopID();
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
    std::snprintf(label, sizeof(label), "Populate %s %d event%s##ravpopulate", kDot, n, n == 1 ? "" : "s");
    const bool none = sel.empty();
    if (none) ImGui::BeginDisabled();
    if (ui::PrimaryButton(label, ImVec2(populate_w, 0.0f))) {
        int events = 0;
        const SoundReport rep = PopulateItemsCounted(sel, &events);
        Remember(rep, events);
        g_count.valid = false;
    }
    if (none) ImGui::EndDisabled();
    ui::ItemTipText(none ? "Select animation items"
                         : "Places each event's sound on the selected items and their linked copies.\n"
                           "Placed sounds you moved stay; deleted ones are not added back. One undo point.");
    ImGui::SameLine();
    if (none) ImGui::BeginDisabled();
    if (ui::SolidButton("Reshuffle##ravreshuffle", ImVec2(reshuffle_w, 0.0f))) Remember(ReshuffleItems(sel), -1);
    if (none) ImGui::EndDisabled();
    ui::ItemTipText(none ? "Select animation items"
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
        // A quiet link (muted, underlined under the mouse), ellipsized to the room before the "?".
        const float w = std::max(ui::Dp(20.0f), ImGui::GetContentRegionAvail().x - help_w - ImGui::GetStyle().ItemSpacing.x);
        const flow::MeasureFn measure = [](const char* b, const char* e) { return ImGui::CalcTextSize(b, e).x; };
        bool              cut = false;
        const std::string shown = flow::Ellipsize(g_last.summary, w, "...", measure, &cut);
        if (ui::LinkButton((shown + "##ravreport").c_str(), ui::kMuted)) ImGui::OpenPopup("##ravreportpop");
        ui::ItemTipText(cut ? (g_last.summary + "\nClick for the details").c_str() : "Click for the details");
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
