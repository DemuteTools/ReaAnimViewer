// SPDX-License-Identifier: MIT
//
// See tagging_assign_ui.h. The decisions (targets, summaries, mixed selections, selection rules, the
// Plays / ladder texts) are assign_view_model.h's; this file draws them with the house theme (ui_theme.h,
// ui_flow.h) and queues every gesture through the picked-gestures mechanism (the window lists the sound
// store by reference, and a gesture replaces it).

#include "tagging_assign_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "assign_view_model.h"
#include "item_rules.h"
#include "library_icons.h"
#include "library_tree_ui.h"
#include "reaper_api.h"
#include "shortcuts.h"  // LoadPrefFloat / SavePrefFloat
#include "sound_assign.h"
#include "sound_auto_assign.h"
#include "sound_populate.h"
#include "sound_project.h"
#include "sound_record.h"
#include "tagging_populate_ui.h"
#include "tagging_session.h"
#include "ui_flow.h"
#include "ui_theme.h"
#include "video_view.h"

namespace rav {

namespace {

namespace av = assign;

const char kDot[] = " \xC2\xB7 ";

constexpr char kWindowId[] = "Sound assignment##ravassign";
constexpr char kPrefX[] = "assign.win_x";  // the window's rect, in dp from the main viewport's corner
constexpr char kPrefY[] = "assign.win_y";
constexpr char kPrefW[] = "assign.win_w";
constexpr char kPrefH[] = "assign.win_h";
constexpr float kDefaultW = 880.0f, kDefaultH = 600.0f;  // dp: the mockup's first-use size
constexpr float kMinW = 460.0f, kMinH = 340.0f;

enum Scope { kScopeProject = 0, kScopeCharacter = 1, kScopeItem = 2 };

// The model signature of a rig (ItemRigInfo::model): its meshes, so the clips of one model are one character.
std::string RigModel(const ItemRigInfo& rig)
{
    return rig.model;
}

struct State {
    bool       open = false;
    int        drawn_frame = -10;
    int        focused_frame = -10;
    bool       rect_known = false;
    ImVec2     pos, size;       // the window's rect as last drawn (screen px)
    ImVec2     saved_pos, saved_size;
    MediaItem* item = nullptr;  // the window's item (follows the Tagging view's unless the user picked one)
    MediaItem* last_model_item = nullptr;
    MediaItem* sel_item = nullptr;  // the item the selection and the folds belong to
    bool       follow = true;
    int        scope = kScopeItem;
    bool       overview = false;
    av::Selection sel;
    std::string   sel_sig;
    std::map<std::string, bool> closed;  // parent row id -> folded (rows start open)
    std::map<std::string, bool> folded;  // section id ("item", "rules", "auto") -> folded
    char        filter[96] = {};
    float       split = -1.0f;  // the tree's width, px
    std::string nav;            // the row the keyboard is on ("item", a leaf key, "P:<parent id>")
    bool        nav_shown = false;
    bool        nav_scroll = false;
    bool        has_last = false;
    av::ChangeText last;
    std::string last_note;      // a view-only note ("Expanded every row ...")
    // A request from the strip.
    bool             request = false;
    MediaItem*       request_item = nullptr;
    std::vector<int> request_blocks;
    bool             focus_request = false;
};
State g;

// ---- Per-frame data ---------------------------------------------------------------------------------

struct Frame {
    const SoundStore*      store = nullptr;
    ItemSounds             rec;
    const SoundCharacter*  ch = nullptr;
    std::vector<Block>     blocks;
    av::Targets            tg;
    std::vector<av::Target> targets;  // by block index
    av::Target             item;      // the "Item values" row
    std::string            item_name, char_name, pool_text;
    bool                   ok = false;
};

struct RecCache {
    MediaItem*         item = nullptr;
    int                count = -1;
    ItemSounds         rec;
    std::vector<Block> blocks;
    bool               valid = false;
    int                pool = 0;
};
RecCache g_cache;

std::string ItemNameOf(MediaItem* item)
{
    if (!item) return std::string();
    if (MediaItem_Take* take = RavTakeOf(item))
        if (const char* nm = GetTakeName(take)) return nm;
    return std::string();
}

std::string LeafTitle(const av::Parent& p, const av::Leaf& l)
{
    return p.leaves.size() > 1 ? p.name + kDot + l.name : l.name;
}

// The item's sound record, rules and pool size, read again when the project's state changes.
void RefreshCache(MediaItem* item)
{
    const int count = GetProjectStateChangeCount(nullptr);
    if (item == g_cache.item && count == g_cache.count) return;
    g_cache.item = item;
    g_cache.count = count;
    g_cache.rec = ReadItemSounds(item);
    ItemRulesRead rr;
    g_cache.valid = ReadItemRules(item, &rr) && rr.valid;
    g_cache.blocks = g_cache.valid ? rr.rules.blocks : std::vector<Block>();
    g_cache.pool = std::max(0, static_cast<int>(PopulateTargets({item}).size()) - 1);
}

bool BuildFrame(MediaItem* item, Frame* f)
{
    if (!item || !ValidatePtr2(nullptr, item, "MediaItem*") || !RavTakeOf(item)) return false;
    f->store = &GetSoundStore(nullptr);
    RefreshCache(item);
    f->rec = g_cache.rec;
    f->blocks = g_cache.blocks;
    const ItemRigInfo rig = ItemRig(item);
    f->ch = CharacterFor(*f->store, rig.skeleton, RigModel(rig));
    f->char_name = f->ch ? f->ch->name : std::string();
    f->item_name = ItemNameOf(item);
    f->pool_text = g_cache.pool > 0 ? "this item + " + std::to_string(g_cache.pool) + " pool cop" + (g_cache.pool == 1 ? "y" : "ies")
                                    : std::string("this item");
    f->tg = av::BuildTargets(f->blocks);
    f->targets.resize(f->blocks.size());
    for (const std::vector<av::Parent>* list : {&f->tg.rules, &f->tg.autos})
        for (const av::Parent& p : *list)
            for (const av::Leaf& l : p.leaves)
                f->targets[static_cast<size_t>(l.block)] =
                    av::MakeTarget(*f->store, f->rec, f->ch, f->blocks[static_cast<size_t>(l.block)], l.key, LeafTitle(p, l));
    f->item = av::MakeItemTarget(*f->store, f->rec, f->targets);
    f->ok = true;
    return true;
}

const av::Target* TargetOf(const Frame& f, const std::string& key)
{
    if (key == av::kItemKey) return &f.item;
    int b = -1;
    if (av::BlockOfKey(key, &b) && b >= 0 && static_cast<size_t>(b) < f.targets.size()) return &f.targets[static_cast<size_t>(b)];
    return nullptr;
}

std::vector<const av::Target*> SelectedTargets(const Frame& f)
{
    std::vector<const av::Target*> out;
    for (const std::string& k : g.sel.keys)
        if (const av::Target* t = TargetOf(f, k)) out.push_back(t);
    return out;
}

// ---- Summary painting -----------------------------------------------------------------------------------

ImU32 ToneInk(av::SegTone t)
{
    switch (t) {
    case av::SegTone::Normal: return ui::kText;
    case av::SegTone::Inherited: return ui::kMuted;
    case av::SegTone::Mixed:
    case av::SegTone::Warn: return ui::kWarn;
    case av::SegTone::Faint: break;
    }
    return ui::kFaint;
}

float SummaryNatWidth(const av::Summary& s)
{
    float w = s.has_dot ? ui::Dp(14.0f) : 0.0f;
    for (const av::Seg& g : s.segs) w += ImGui::CalcTextSize(g.text.c_str()).x;
    return w;
}

// Paints the summary in [x, x + max_w] on a line starting at y (right aligned when it fits and asked to);
// a summary too long ends with "...". True when cut.
bool PaintSummary(ImDrawList* dl, float x, float y, float max_w, const av::Summary& s, bool right_align)
{
    const float nat = SummaryNatWidth(s);
    float       x0 = x;
    if (right_align && nat < max_w) x0 = x + max_w - nat;
    const float xr = x + max_w;
    if (s.has_dot) {
        if (x0 + ui::Dp(8.0f) <= xr)
            dl->AddCircleFilled(ImVec2(x0 + ui::Dp(4.0f), y + ImGui::GetTextLineHeight() * 0.5f), ui::Dp(4.0f), ui::LibDot(s.dot_color));
        x0 += ui::Dp(14.0f);
    }
    for (const av::Seg& g : s.segs) {
        const float tw = ImGui::CalcTextSize(g.text.c_str()).x;
        if (x0 + tw <= xr) {
            dl->AddText(ImVec2(x0, y), ToneInk(g.tone), g.text.c_str());
            x0 += tw;
        } else {
            const std::string cut = ui::EllipsizeToWidth(ImGui::GetFont(), g.text, std::max(0.0f, xr - x0));
            dl->AddText(ImVec2(x0, y), ToneInk(g.tone), cut.c_str());
            return true;
        }
    }
    return false;
}

// ---- Gestures ---------------------------------------------------------------------------------------------

void SetLast(const std::string& undo, const std::string& where, const std::string& what, int n)
{
    g.last = av::LastChange(undo, where, what, n);
    g.has_last = true;
    g.last_note.clear();
}

// The rids of the selected leaf targets that have one, and the blocks of those that have none yet.
void RulesOf(const Frame& f, const std::vector<const av::Target*>& sel, std::vector<std::string>* rids, std::vector<int>* blocks)
{
    for (const av::Target* t : sel) {
        int b = -1;
        if (!av::BlockOfKey(t->key, &b) || b < 0 || static_cast<size_t>(b) >= f.blocks.size()) continue;
        const std::string& rid = f.blocks[static_cast<size_t>(b)].rid;
        if (rid.empty()) blocks->push_back(b);
        else rids->push_back(rid);
    }
}

void PickLibrary(const Frame& f, const std::vector<const av::Target*>& sel, const std::string& arg, const std::string& what)
{
    MediaItem*               item = g.item;
    std::vector<std::string> rids;
    std::vector<int>         blocks;
    RulesOf(f, sel, &rids, &blocks);
    LibState    state = LibState::Inherit;
    std::string uid;
    if (arg == kLibNoneWord) state = LibState::None;
    else if (!arg.empty()) {
        state = LibState::Library;
        uid = arg;
    }
    const std::string where = f.item_name;
    const int         n = static_cast<int>(sel.size());
    PickSoundGesture([item, rids, blocks, state, uid, what, where, n]() {
        const SoundReport rep = SetRulesAssignLibrary(item, rids, state, uid, blocks);
        SetLast("Set rule library", where, what, n);
        return rep;
    });
}

void PickValue(const Frame& f, const std::vector<const av::Target*>& sel, bool item_row, const std::string& group,
               const std::string& value, const std::string& what)
{
    MediaItem*               item = g.item;
    std::vector<std::string> rids;
    std::vector<int>         blocks;
    if (!item_row) RulesOf(f, sel, &rids, &blocks);
    const std::string where = f.item_name;
    const int         n = static_cast<int>(sel.size());
    PickSoundGesture([item, rids, blocks, group, value, what, where, n, item_row]() {
        const SoundReport rep = item_row ? SetSoundValue(item, SoundValueLevel::Item, -1, 0.0, group, value)
                                         : SetRulesAssignValue(item, rids, group, value, blocks);
        SetLast("Set sound value", where, what, n);
        return rep;
    });
}

// ---- Tree rows ---------------------------------------------------------------------------------------------

enum class RowT { Section, ItemRow, Parent, Leaf, Note };

struct Row {
    RowT                  t = RowT::Note;
    std::string           id;      // section id / parent id / leaf key / "item"
    std::string           nav;     // keyboard id
    const av::Parent*     p = nullptr;
    const av::Leaf*       l = nullptr;
    std::string           title, scope_text, note;
    int                   count = -1;
    std::vector<const av::Parent*> section_parents;  // a section's parents (Expand all)
};

bool IsClosed(const std::string& id)
{
    const auto it = g.closed.find(id);
    return it != g.closed.end() && it->second;
}
bool IsFolded(const std::string& id)
{
    const auto it = g.folded.find(id);
    return it != g.folded.end() && it->second;
}

bool ParentMatches(const av::Parent& p, const std::string& filter)
{
    if (av::FilterMatch(filter, p.name)) return true;
    for (const av::Leaf& l : p.leaves)
        if (av::FilterMatch(filter, l.name)) return true;
    return false;
}

std::vector<Row> BuildRows(const Frame& f)
{
    std::vector<Row> rows;
    const std::string filter = g.filter;
    if (av::FilterMatch(filter, "Item values")) {
        Row s;
        s.t = RowT::Section;
        s.id = "item";
        s.title = "Item";
        s.scope_text = f.pool_text;
        rows.push_back(s);
        if (!IsFolded("item")) {
            Row r;
            r.t = RowT::ItemRow;
            r.id = av::kItemKey;
            r.nav = av::kItemKey;
            rows.push_back(r);
        }
    }
    auto section = [&](const char* id, const char* title, const std::vector<av::Parent>& parents, const std::string& scope,
                       const char* empty) {
        std::vector<const av::Parent*> shown;
        for (const av::Parent& p : parents)
            if (ParentMatches(p, filter)) shown.push_back(&p);
        if (!filter.empty() && shown.empty()) return;
        Row s;
        s.t = RowT::Section;
        s.id = id;
        s.title = title;
        s.scope_text = scope;
        s.count = static_cast<int>(parents.size());
        s.section_parents = shown;
        rows.push_back(s);
        if (IsFolded(id)) return;
        if (parents.empty()) {
            Row n;
            n.t = RowT::Note;
            n.note = empty;
            rows.push_back(n);
        }
        for (const av::Parent* p : shown) {
            Row r;
            r.t = RowT::Parent;
            r.id = p->id;
            r.nav = "P:" + p->id;
            r.p = p;
            rows.push_back(r);
            if (IsClosed(p->id) || p->leaves.size() < 2) continue;
            for (const av::Leaf& l : p->leaves) {
                Row lr;
                lr.t = RowT::Leaf;
                lr.id = l.key;
                lr.nav = l.key;
                lr.p = p;
                lr.l = &l;
                rows.push_back(lr);
            }
        }
    };
    section("rules", "Bone rules", f.tg.rules, f.pool_text, "No bone rule on this item.");
    section("auto", "Auto detection", f.tg.autos, "this item's auto blocks", "No auto type ticked.");
    return rows;
}

// The keys of the rows a click can land on (leaves, a single-leaf parent's leaf, the Item row), in order.
std::vector<std::string> VisibleKeys(const std::vector<Row>& rows)
{
    std::vector<std::string> out;
    for (const Row& r : rows) {
        if (r.t == RowT::ItemRow) out.push_back(av::kItemKey);
        else if (r.t == RowT::Leaf) out.push_back(r.l->key);
        else if (r.t == RowT::Parent && r.p->leaves.size() == 1) out.push_back(r.p->leaves[0].key);
    }
    return out;
}

std::vector<std::string> LeafKeysOf(const av::Parent& p)
{
    std::vector<std::string> out;
    for (const av::Leaf& l : p.leaves) out.push_back(l.key);
    return out;
}

std::vector<const av::Target*> TargetsOfParent(const Frame& f, const av::Parent& p)
{
    std::vector<const av::Target*> out;
    for (const av::Leaf& l : p.leaves) out.push_back(&f.targets[static_cast<size_t>(l.block)]);
    return out;
}

void NoteView(const std::string& text)
{
    g.last_note = text;
    g.has_last = false;
}

// "Expand all" / "Collapse all": view only, no undo point.
void SectionAll(const std::string& id, const std::vector<const av::Parent*>& parents)
{
    bool all_open = !IsFolded(id);
    for (const av::Parent* p : parents)
        if (IsClosed(p->id)) all_open = false;
    g.folded[id] = false;
    for (const av::Parent* p : parents) g.closed[p->id] = all_open;
    NoteView(std::string(all_open ? "Collapsed" : "Expanded") + " every row of " + (id == "rules" ? "Bone rules" : "Auto detection") +
             " (view only, no undo point).");
}

void ActivateRow(const Row& r, const std::vector<std::string>& visible, bool shift, bool ctrl)
{
    if (r.t == RowT::ItemRow) av::ClickRow(g.sel, av::kItemKey, shift, ctrl, visible);
    else if (r.t == RowT::Leaf) av::ClickRow(g.sel, r.l->key, shift, ctrl, visible);
    else if (r.t == RowT::Parent) {
        if (r.p->leaves.size() == 1) av::ClickRow(g.sel, r.p->leaves[0].key, shift, ctrl, visible);
        else av::ClickParent(g.sel, LeafKeysOf(*r.p), shift || ctrl);
    }
}

void DrawSectionHeader(const Row& r)
{
    ImGui::PushID(r.id.c_str());
    const float  h = ui::SectionHeaderH();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const bool   folded = IsFolded(r.id);
    float        x = p.x;
    ImGui::SetCursorScreenPos(ImVec2(x, p.y + (h - ui::Dp(18.0f)) * 0.5f));
    if (ui::Chevron("##fold", !folded)) g.folded[r.id] = !folded;
    ui::ItemTipText(folded ? "Open this section" : "Fold this section");
    x += ui::Dp(22.0f);
    // "Expand all / Collapse all" at the right, for the sections that hold parent rows.
    float right = p.x + w;
    if (!r.section_parents.empty() || r.id == "rules" || r.id == "auto") {
        bool all_open = !folded;
        for (const av::Parent* q : r.section_parents)
            if (IsClosed(q->id)) all_open = false;
        const char* label = all_open ? "\xE2\x8A\x9F Collapse all##all" : "\xE2\x8A\x9E Expand all##all";
        const float bw = ui::ButtonWidth(label);
        if (right - bw >= x + ui::Dp(90.0f) && !r.section_parents.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(right - bw, p.y + (h - ui::ControlH()) * 0.5f));
            if (ui::SolidButton(label)) SectionAll(r.id, r.section_parents);
            right -= bw + ui::Dp(6.0f);
        }
    }
    {
        ui::FontScope fs(ui::FontRole::Small);
        std::string   up = r.title;
        for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
        dl->AddText(ImVec2(x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), ui::kMuted, up.c_str());
        x += ImGui::CalcTextSize(up.c_str()).x + ui::Dp(6.0f);
    }
    if (r.count >= 0) {
        ui::FontScope fs(ui::FontRole::MonoTiny);
        const std::string n = std::to_string(r.count);
        dl->AddText(ImVec2(x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), ui::kFaint, n.c_str());
        x += ImGui::CalcTextSize(n.c_str()).x + ui::Dp(8.0f);
    }
    if (x < right - ui::Dp(30.0f)) {
        ui::FontScope fs(ui::FontRole::Small);
        ui::EllipsisTextTip(dl, ImVec2(x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), right, ui::kFaint, r.scope_text);
    }
    dl->AddLine(ImVec2(p.x, p.y + h - 1.0f), ImVec2(p.x + w, p.y + h - 1.0f), folded ? 0 : ui::kStroke);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
}

// Name column of a row, ellipsized: [x, x + max_w].
void RowName(ImDrawList* dl, float x, float y, float max_w, ImU32 col, const std::string& name)
{
    ui::EllipsisTextTip(dl, ImVec2(x, y), x + max_w, col, name);
}

void DrawTreeRows(const Frame& f, const std::vector<Row>& rows)
{
    const std::vector<std::string> visible = VisibleKeys(rows);
    const ImGuiIO&                 io = ImGui::GetIO();
    for (const Row& r : rows) {
        if (r.t == RowT::Section) {
            if (&r != &rows[0]) ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
            DrawSectionHeader(r);
            continue;
        }
        if (r.t == RowT::Note) {
            ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
            ui::SubText(r.note.c_str());
            continue;
        }
        ImGui::PushID(r.nav.c_str());
        const float  rh = ui::RowH();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        const float  ty = std::floor(p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f);
        const float  pad = ui::Dp(6.0f);
        const bool   is_nav = g.nav_shown && g.nav == r.nav;
        if (is_nav && g.nav_scroll) {
            ImGui::SetScrollHereY(0.5f);
            g.nav_scroll = false;
        }
        const unsigned bits = is_nav ? ui::kRowCurrent : 0u;

        if (r.t == RowT::Parent) {
            const std::vector<std::string> keys = LeafKeysOf(*r.p);
            const int                      state = av::ParentState(g.sel, keys);
            const bool                     multi = r.p->leaves.size() > 1;
            const std::vector<const av::Target*> tl = TargetsOfParent(f, *r.p);
            const av::Summary                    sum = av::Summarize(*f.store, tl, true);
            ImGui::SetNextItemAllowOverlap();
            if (ui::SelectRow("##row", state == 1, bits, rh, ImGuiSelectableFlags_AllowOverlap, ui::kRadiusMd)) {
                g.nav = r.nav;
                ActivateRow(r, visible, io.KeyShift, io.KeyCtrl);
            }
            float x = p.x + pad;
            if (multi) {
                ImGui::SetCursorScreenPos(ImVec2(x, p.y + (rh - ui::Dp(18.0f)) * 0.5f));
                if (ui::Chevron("##chev", !IsClosed(r.id))) g.closed[r.id] = !IsClosed(r.id);
                ui::ItemTipText(IsClosed(r.id) ? "Show the sides" : "Hide the sides");
            }
            x += ui::Dp(18.0f) + ui::Dp(6.0f);
            ImGui::SetCursorScreenPos(ImVec2(x, p.y + (rh - ImGui::GetFrameHeight()) * 0.5f));
            if (ui::TriCheckbox("##chk", state)) {
                g.nav = r.nav;
                if (multi) av::ClickParent(g.sel, keys, true);
                else av::ClickRow(g.sel, keys[0], false, true, visible);
            }
            ui::ItemTipText(multi ? "Select both sides" : "Select this rule");
            x += ui::Dp(14.0f) + ui::Dp(7.0f);
            const float name_w = std::min(ImGui::CalcTextSize(r.p->name.c_str()).x + 1.0f, (p.x + w - x) * 0.45f);
            RowName(dl, x, ty, name_w, ui::kText, r.p->name);
            x += name_w + ui::Dp(6.0f);
            if (sum.blocked) {
                ui::AddTextIn(dl, ui::FontSemibold(), ImVec2(x, ty), ui::kWarn, "!");
                x += ui::Dp(10.0f);
            }
            if (x < p.x + w - pad - ui::Dp(20.0f)) {
                if (PaintSummary(dl, x, ty, p.x + w - pad - x, sum, true)) {
                    ImGui::SetCursorScreenPos(ImVec2(x, p.y));
                    ImGui::Dummy(ImVec2(p.x + w - pad - x, rh));
                    ui::ItemTipText(sum.Plain().c_str());
                }
            }
        } else if (r.t == RowT::Leaf) {
            const av::Target& t = f.targets[static_cast<size_t>(r.l->block)];
            const bool        sel = g.sel.Has(r.l->key);
            const av::Summary sum = av::Summarize(*f.store, {&t}, true);
            ImGui::SetNextItemAllowOverlap();
            if (ui::SelectRow("##row", sel, bits, rh, ImGuiSelectableFlags_AllowOverlap, ui::kRadiusMd)) {
                g.nav = r.nav;
                ActivateRow(r, visible, io.KeyShift, io.KeyCtrl);
            }
            const float x = p.x + ui::Dp(50.0f);
            const float name_w = std::min(ui::Dp(72.0f), std::max(ui::Dp(30.0f), (p.x + w - x) * 0.3f));
            RowName(dl, x, ty, name_w, sel ? ui::kText : ui::kMuted, r.l->name);
            const float sx = x + name_w + ui::Dp(8.0f);
            if (sx < p.x + w - pad - ui::Dp(20.0f)) {
                if (PaintSummary(dl, sx, ty, p.x + w - pad - sx, sum, true)) {
                    ImGui::SetCursorScreenPos(ImVec2(sx, p.y));
                    ImGui::Dummy(ImVec2(p.x + w - pad - sx, rh));
                    ui::ItemTipText(sum.Plain().c_str());
                }
            }
        } else {  // the Item values row
            const bool sel = g.sel.Has(av::kItemKey);
            ImGui::SetNextItemAllowOverlap();
            if (ui::SelectRow("##row", sel, bits, rh, ImGuiSelectableFlags_AllowOverlap, ui::kRadiusMd)) {
                g.nav = r.nav;
                ActivateRow(r, visible, io.KeyShift, io.KeyCtrl);
            }
            const av::Summary sum = av::SummarizeItem(*f.store, f.item);
            float             x = p.x + pad + ui::Dp(4.0f);
            const float       name_w = std::min(ImGui::CalcTextSize("Item values").x + 1.0f, (p.x + w - x) * 0.45f);
            RowName(dl, x, ty, name_w, ui::kText, "Item values");
            x += name_w + ui::Dp(8.0f);
            if (x < p.x + w - pad - ui::Dp(20.0f)) PaintSummary(dl, x, ty, p.x + w - pad - x, sum, true);
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
    }
}

// Up / Down move the keyboard row, Left / Right fold a parent, Enter / Space click it.
void HandleTreeKeys(const std::vector<Row>& rows)
{
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput) return;
    std::vector<const Row*> navs;
    for (const Row& r : rows)
        if (!r.nav.empty()) navs.push_back(&r);
    if (navs.empty()) return;
    int cur = -1;
    for (size_t i = 0; i < navs.size(); ++i)
        if (navs[i]->nav == g.nav) cur = static_cast<int>(i);
    const ImGuiIO& io = ImGui::GetIO();
    const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow), up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
    if (down || up) {
        if (cur < 0) cur = 0;
        else cur = std::max(0, std::min(static_cast<int>(navs.size()) - 1, cur + (down ? 1 : -1)));
        g.nav = navs[static_cast<size_t>(cur)]->nav;
        g.nav_shown = true;
        g.nav_scroll = true;
        return;
    }
    if (cur < 0) return;
    const Row& r = *navs[static_cast<size_t>(cur)];
    if (r.t == RowT::Parent && r.p->leaves.size() > 1) {
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) g.closed[r.id] = false;
        else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) g.closed[r.id] = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Space))
        ActivateRow(r, VisibleKeys(rows), io.KeyShift, io.KeyCtrl);
}

// ---- Details ----------------------------------------------------------------------------------------------

void OpenLibraryView(const av::Target* t)
{
    if (t && t->lib) SelectLibraryRow(t->lib->uid, LibraryRow{std::string(), t->lib->root});
    SetViewMode(ViewMode::Library);
}

void Banner(bool warn, const std::string& text, const av::Target* open_for)
{
    if (warn) {
        ui::ConfirmBand band;
        ui::BeginConfirmBand(band, 0.0f, true);
        ui::ToneText(text, ui::kWarn, true);
        if (open_for && ui::SolidButton("Open Library view##banner")) OpenLibraryView(open_for);
        ui::EndConfirmBand(band);
    } else {
        ui::Well well;
        ui::BeginWell(well);
        ui::ToneText(text, ui::kMuted, true);
        ui::EndWell(well);
    }
}

std::string LibraryLabel(const SoundStore& store, const std::string& arg)
{
    if (arg.empty()) return "inherit";
    if (arg == kLibNoneWord) return "None";
    const SoundLibrary* l = FindLibrary(store, arg);
    return l ? l->name : std::string("(gone)");
}

// "inherit . FS Hero (Project . Feet)" for the targets (their inherited assignment); "Not set (nothing placed)".
std::string InheritLabel(const SoundStore& store, const std::vector<const av::Target*>& sel)
{
    const EffectiveAssign& a = sel[0]->inherited;
    for (const av::Target* t : sel)
        if (t->inherited.library != a.library || t->inherited.none != a.none || t->inherited.from != a.from) return "inherit \xC2\xB7 mixed";
    if (a.level == AssignLevel::Unset) return "Not set (nothing placed)";
    const std::string what = a.none ? std::string("None") : LibraryLabel(store, a.library);
    return "inherit \xC2\xB7 " + what + " (" + a.from + ")";
}

void DrawLibraryRow(const Frame& f, const std::vector<const av::Target*>& sel, const av::Mix& mix)
{
    const SoundStore& store = *f.store;
    std::string       own = av::OwnLibraryArg(*sel[0]);
    const bool        own_mixed = mix.own_settings > 1;
    const float       w = ui::LabelRow("Library", ui::Dp(140.0f));
    std::string       preview;
    if (own_mixed) preview = "mixed (" + std::to_string(mix.own_settings) + " settings)";
    else if (own.empty()) preview = InheritLabel(store, sel);
    else if (own == kLibNoneWord) preview = "None (place nothing)";
    else preview = LibraryLabel(store, own);
    if (ui::BeginComboStyled("##assignlib", preview.c_str(), w, 0, own_mixed ? ui::TagTone::Warn : ui::TagTone::Neutral)) {
        if (own_mixed) {
            ImGui::BeginDisabled();
            ImGui::Selectable((preview + "##mixed").c_str(), true);
            ImGui::EndDisabled();
        }
        const std::string inh = InheritLabel(store, sel);
        if (ImGui::Selectable((inh + "##inherit").c_str(), !own_mixed && own.empty()) && (own_mixed || !own.empty()))
            PickLibrary(f, sel, std::string(), "Library = inherit");
        if (ImGui::Selectable("None (place nothing)##none", !own_mixed && own == kLibNoneWord) && (own_mixed || own != kLibNoneWord))
            PickLibrary(f, sel, kLibNoneWord, "Library = None");
        for (const SoundLibrary& L : store.libraries) {
            std::string label = L.name;
            bool        has_sound = false;
            for (const SoundNode& n : L.nodes)
                if (n.type == SoundNodeType::Sound) has_sound = true;
            if (!has_sound) label += " (empty)";
            else if (CheckLibrary(store, L).blocked) label += " (blocked)";
            const bool cur = !own_mixed && own == L.uid;
            if (ImGui::Selectable((label + "##" + L.uid).c_str(), cur) && !cur) PickLibrary(f, sel, L.uid, "Library = " + L.name);
        }
        ui::EndComboStyled();
    }
    // Where the setting is: set here, from a kit, inherited.
    if (sel.size() == 1 && !mix.lib) {
        const av::Target& t = *sel[0];
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        if (t.OwnLib()) {
            ui::Tag(t.ea.kit && t.lib ? ("from " + t.lib->name + "'s kit").c_str() : "set here", ui::TagTone::Project);
        } else {
            ui::ToneText(t.Unset() ? std::string("nothing set at any level") : "inherited from " + t.ea.from, ui::kFaint, true);
        }
    }
}

// One Switch Group: its step number, name, tags, the chips, the event note.
void DrawGroupRow(const Frame& f, const std::vector<const av::Target*>& sel, bool item_row, const std::string& group_uid, int step,
                  bool first)
{
    const av::GroupState* g0 = nullptr;
    for (const av::Target* t : sel)
        if ((g0 = t->Group(group_uid))) break;
    if (!g0) return;
    ImGui::PushID(group_uid.c_str());
    if (!first) ImGui::Separator();
    if (g0->deleted) {
        ImGui::AlignTextToFramePadding();
        char n[16];
        std::snprintf(n, sizeof(n), "%d", step);
        ui::ToneText(n, ui::kFaint);
        ImGui::SameLine();
        ui::ToneText(g0->group_name.empty() ? std::string("(deleted group)") : g0->group_name, ui::kMuted);
        ImGui::SameLine();
        ui::Tag("group deleted", ui::TagTone::Warn);
        ui::ToneText("The Switch Group no longer exists. Re-create it or change the Switch in the Library view.", ui::kFaint, true);
        ImGui::PopID();
        return;
    }
    const SwitchGroup*       grp = FindGroup(*f.store, group_uid);
    std::vector<std::string> own;
    int                      unreached = 0;
    bool                     inh_same = true;
    for (const av::Target* t : sel) {
        const av::GroupState* gs = t->Group(group_uid);
        own.push_back(gs ? gs->own_uid : std::string());
        if (gs && !gs->reached) ++unreached;
        if (gs && (gs->inherit_name != g0->inherit_name || gs->inherit_from != g0->inherit_from)) inh_same = false;
    }
    const bool mixed = std::set<std::string>(own.begin(), own.end()).size() > 1;
    auto       count_of = [&](const std::string& v) {
        return static_cast<int>(std::count(own.begin(), own.end(), v));
    };
    // Header line.
    ImGui::AlignTextToFramePadding();
    char n[16];
    std::snprintf(n, sizeof(n), "%d", step);
    {
        ui::FontScope fs(ui::FontRole::MonoTiny);
        ui::ToneText(n, ui::kFaint);
    }
    ImGui::SameLine();
    {
        ui::FontScope fs(ui::FontRole::Semibold);
        ui::ToneText(g0->group_name, ui::kMuted);
    }
    if (mixed) {
        ImGui::SameLine();
        ui::Tag("mixed", ui::TagTone::Warn);
    }
    if (unreached > 0) {
        const av::GroupState* un = nullptr;
        for (const av::Target* t : sel) {
            const av::GroupState* gs = t->Group(group_uid);
            if (gs && !gs->reached) {
                un = gs;
                break;
            }
        }
        std::string why = un->not_reached;
        if (unreached < static_cast<int>(sel.size())) why += " (" + std::to_string(unreached) + " of " + std::to_string(sel.size()) + ")";
        ImGui::SameLine();
        ui::ToneText(why, ui::kFaint);
    }
    // Chips.
    ImGui::Indent(ui::Dp(22.0f));
    if (unreached == static_cast<int>(sel.size())) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
    auto chip = [&](const std::string& label, const std::string& value, bool on, bool is_first) {
        const float cw = ui::TextWidth(label.c_str()) + ui::Dp(14.0f);
        if (!is_first) ui::SameLineIfFits(cw);
        if (ui::ChipButton((label + "##" + value).c_str(), ImVec2(0.0f, 0.0f), on) && !on) {
            std::string what = g0->group_name + " = " + (value.empty() ? std::string("inherit") : [&] {
                for (const SwitchValue& v : grp->values)
                    if (v.uid == value) return v.name;
                return value;
            }());
            PickValue(f, sel, item_row, group_uid, value, what);
        }
    };
    {
        const std::string inh = "inherit \xC2\xB7 " + (inh_same ? g0->inherit_name : std::string("mixed")) + " (" +
                                (inh_same ? g0->inherit_from : std::string("several")) + ")";
        chip(inh + (mixed && count_of(std::string()) ? "  " + std::to_string(count_of(std::string())) : std::string()), std::string(),
             !mixed && own[0].empty(), true);
    }
    if (grp)
        for (const SwitchValue& v : grp->values) {
            const int c = count_of(v.uid);
            chip(v.name + (mixed && c ? "  " + std::to_string(c) : std::string()), v.uid, !mixed && own[0] == v.uid, false);
        }
    if (unreached == static_cast<int>(sel.size())) ImGui::PopStyleVar();
    // Events that set their own value of this group.
    if (!item_row) {
        std::vector<std::string> rids;
        std::vector<int>         blocks;
        RulesOf(f, sel, &rids, &blocks);
        const int ev = av::EventOverrideCount(f.rec, rids, group_uid);
        if (ev > 0)
            ui::ToneText(std::to_string(ev) + " event" + (ev > 1 ? "s" : "") + " set their own " + g0->group_name +
                             " (change them in the event inspector)",
                         ui::kFaint, true);
    }
    ImGui::Unindent(ui::Dp(22.0f));
    ImGui::PopID();
}

void DrawPlays(const std::vector<const av::Target*>& sel, bool item_scope_note)
{
    (void)item_scope_note;
    const std::vector<av::PlaysLine> lines = av::PlaysLines(sel);
    bool                             blocked = false;
    for (const av::Target* t : sel)
        if (t->lib_blocked) blocked = true;
    ui::ConfirmBand band;
    ui::Well        well;
    if (blocked) ui::BeginConfirmBand(band, 0.0f, true);
    else ui::BeginWell(well);
    ui::GroupCaption("Plays");
    const size_t shown = std::min<size_t>(lines.size(), 4);
    for (size_t i = 0; i < shown; ++i) {
        ImGui::PushID(static_cast<int>(i));
        ui::ToneText(lines[i].text, ui::kText, true);
        if (lines.size() > 1) {
            std::string who;
            if (lines[i].titles.size() > 2) who = std::to_string(lines[i].titles.size()) + " targets";
            else
                for (const std::string& t : lines[i].titles) who += (who.empty() ? "" : ", ") + t;
            ui::ToneText(who, ui::kFaint, true);
        }
        ImGui::PopID();
    }
    if (lines.size() > shown) ui::ToneText("+ " + std::to_string(lines.size() - shown) + " more paths", ui::kFaint);
    if (sel.size() == 1 && sel[0]->Plays()) ui::ToneText(av::WhyText(*sel[0]), ui::kFaint, true);
    if (blocked) ui::ToneText("Populate reports \"Blocked\" for these events.", ui::kFaint, true);
    if (blocked) ui::EndConfirmBand(band);
    else ui::EndWell(well);
}

void LadderRowDraw(const SoundStore& store, const std::string& label, bool hit, const std::function<void(ImDrawList*, float, float, float)>& value)
{
    const float  h = ui::ControlH() + ui::Dp(4.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    (void)store;
    if (hit) ui::RowBackground(dl, p, ImVec2(p.x + w, p.y + h), ui::kRowLit, ui::kRadiusSm);
    const float ty = std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
    const float kw = std::min(ui::Dp(128.0f), w * 0.4f);
    ui::EllipsisTextTip(dl, ImVec2(p.x + ui::Dp(6.0f), ty), p.x + kw, hit ? ui::kAccentInk : ui::kMuted, label);
    value(dl, p.x + kw + ui::Dp(8.0f), ty, p.x + w - ui::Dp(6.0f));
    ImGui::Dummy(ImVec2(w, h));
}

void DrawLadder(const Frame& f, const av::Target& t)
{
    const av::Ladder ld = av::BuildLadder(*f.store, f.ch, t);
    ui::Section      s;
    ui::SectionOpts  o;
    o.suffix = "top = general \xC2\xB7 bottom = precise";
    if (ui::BeginSection(s, "Where the library comes from", o)) {
        std::string kits;
        for (const std::string& k : ld.kits) kits += (kits.empty() ? "" : ", ") + k;
        LadderRowDraw(*f.store, "Library kits", false, [&](ImDrawList* dl, float x, float y, float right) {
            ui::EllipsisTextTip(dl, ImVec2(x, y), right, ui::kFaint, ld.kits.empty() ? std::string("none") : kits + " suggest " + ld.part_label);
        });
        for (const av::LadderRow& r : ld.rows) {
            LadderRowDraw(*f.store, r.label, r.hit, [&](ImDrawList* dl, float x, float y, float right) {
                if (!r.set) {
                    dl->AddText(ImVec2(x, y), ui::kFaint, "\xE2\x80\x94");
                    return;
                }
                std::string name = "None";
                if (!r.none) {
                    const SoundLibrary* l = FindLibrary(*f.store, r.library);
                    name = l ? l->name : std::string("(gone)");
                    if (l) dl->AddCircleFilled(ImVec2(x + ui::Dp(4.0f), y + ImGui::GetTextLineHeight() * 0.5f), ui::Dp(4.0f), ui::LibDot(l->color));
                }
                const float nx = x + (r.none ? 0.0f : ui::Dp(14.0f));
                std::string text = name + (r.kit ? "  (kit)" : "") + (r.hit ? "  \xE2\x86\x90 plays" : "");
                ui::EllipsisTextTip(dl, ImVec2(nx, y), right, ui::kText, text);
            });
        }
        LadderRowDraw(*f.store, "event", false, [&](ImDrawList* dl, float x, float y, float right) {
            ui::EllipsisTextTip(dl, ImVec2(x, y), right, ui::kFaint, "values only, in the event inspector");
        });
    }
    ui::EndSection(s);
}

void DrawDetails(const Frame& f)
{
    std::vector<const av::Target*> sel = SelectedTargets(f);
    if (sel.empty()) {
        ImGui::Dummy(ImVec2(0.0f, ui::Dp(16.0f)));
        ui::ToneText("Select a row on the left.", ui::kMuted, true);
        ui::ToneText("Shift-click picks a range, Ctrl-click adds one.", ui::kMuted, true);
        return;
    }
    const bool item_row = sel[0]->is_item;
    const av::Mix mix = item_row ? av::Mix() : av::MixOf(sel);
    // Title line.
    {
        ui::FontScope fs(ui::FontRole::PaneTitle);
        const std::string title = av::SelectionTitle(f.tg, sel);
        ui::ToneText(title, ui::kText, true);
    }
    if (mix.lib || mix.groups) {
        ImGui::SameLine();
        ui::Tag("mixed", ui::TagTone::Warn);
    }
    bool any_rule = false;
    for (const av::Target* t : sel) {
        int b = -1;
        if (av::BlockOfKey(t->key, &b) && b >= 0 && static_cast<size_t>(b) < f.blocks.size() && !IsAutoBlock(f.blocks[static_cast<size_t>(b)])) any_rule = true;
    }
    ui::ToneText(av::ScopeLine(item_row, any_rule, f.pool_text, f.char_name), ui::kMuted, true);

    if (!item_row) {
        DrawLibraryRow(f, sel, mix);
    }
    if (mix.lib) {
        Banner(false,
               "These targets use " + std::to_string(std::set<std::string>([&] {
                   std::set<std::string> libs;
                   for (const av::Target* t : sel) libs.insert(t->ea.none ? "none" : t->ea.library);
                   return libs;
               }()).size()) + " different libraries. Pick one library above to edit their Switch values together.",
               nullptr);
        DrawPlays(sel, false);
        return;
    }
    const av::Target& first = *sel[0];
    if (!item_row && !first.lib) {
        ui::ToneText((first.Unset() ? std::string("Nothing set: no level above gives a library. ") : std::string("No sound. ")) +
                         "Populate places nothing for " + (sel.size() > 1 ? "these targets." : "this target."),
                     ui::kMuted, true);
        return;
    }
    if (!item_row && first.lib_empty) {
        Banner(true, first.lib->name + " is empty: it has no sound yet. Build it in the Library view; nothing is placed until then.", &first);
        return;
    }
    if (!item_row && first.lib_blocked)
        Banner(true, first.lib->name + " is blocked: " + first.blocked_reason + ". Nothing is placed from this library until it is fixed.", &first);

    // Switch values.
    std::vector<std::string> groups;
    for (const av::GroupState& gs : (item_row ? f.item.groups : first.groups)) groups.push_back(gs.group_uid);
    ui::Section     s;
    ui::SectionOpts o;
    o.suffix = item_row ? "all libraries of this item" : "in path order";
    if (ui::BeginSection(s, "Switch values", o)) {
        if (groups.empty()) ui::SubText("No library here switches on a group.");
        for (size_t i = 0; i < groups.size(); ++i) DrawGroupRow(f, sel, item_row, groups[i], static_cast<int>(i) + 1, i == 0);
    }
    ui::EndSection(s);
    if (item_row) return;
    DrawPlays(sel, false);
    if (sel.size() == 1) DrawLadder(f, first);
    // Where the sounds land in the Library.
    std::vector<std::string> tracks;
    for (const av::Target* t : sel)
        if (t->Plays()) {
            const std::string name = av::TrackNameOf(*t);
            if (std::find(tracks.begin(), tracks.end(), name) == tracks.end()) tracks.push_back(name);
        }
    if (!tracks.empty()) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();
        ImGui::AlignTextToFramePadding();
        ui::ToneText("Lands in", ui::kMuted);
        ImGui::SameLine();
        ui::ToneText("RAV Library \xE2\x80\xBA " + (tracks.size() == 1 ? tracks[0] : std::to_string(tracks.size()) + " tracks"), ui::kText, true);
        ui::ToneText("set per sound in the Library view", ui::kFaint, true);
        ImGui::EndGroup();
        ui::DashedRect(ImGui::GetWindowDrawList(), ImVec2(p0.x - ui::Dp(4.0f), p0.y - ui::Dp(3.0f)),
                       ImVec2(ImGui::GetItemRectMax().x + ui::Dp(4.0f), ImGui::GetItemRectMax().y + ui::Dp(3.0f)), ui::kStrokeStrong,
                       ui::kRadiusMd);
    }
}

// ---- Overview -----------------------------------------------------------------------------------------------

void DrawOverview(const Frame& f, float height)
{
    if (!ui::BeginPane("##assignmx", ImVec2(0.0f, height), ui::kSurface, 0, ImGuiWindowFlags_HorizontalScrollbar)) {
        ui::EndPane();
        return;
    }
    const std::vector<std::string> groups = av::OverviewGroups(*f.store, f.targets);
    const float  avail = std::max(ui::Dp(420.0f), ImGui::GetContentRegionAvail().x);
    const float  c0 = ui::Dp(150.0f), c1 = ui::Dp(150.0f);
    const float  cg = groups.empty() ? 0.0f : std::max(ui::Dp(84.0f), (avail - c0 - c1) / static_cast<float>(groups.size()));
    const float  total = c0 + c1 + cg * static_cast<float>(groups.size());
    const float  rh = ui::Dp(28.0f);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    {  // header
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ui::FontScope fs(ui::FontRole::Tiny);
        auto head = [&](float x, const std::string& t) {
            std::string up = t;
            for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            dl->AddText(ImVec2(p.x + x + ui::Dp(8.0f), p.y + ui::Dp(5.0f)), ui::kFaint, up.c_str());
        };
        head(0.0f, "Target");
        head(c0, "Library");
        for (size_t i = 0; i < groups.size(); ++i) {
            const SwitchGroup* g2 = FindGroup(*f.store, groups[i]);
            head(c0 + c1 + cg * static_cast<float>(i), g2 ? g2->name : std::string("?"));
        }
        dl->AddLine(ImVec2(p.x, p.y + ui::Dp(22.0f)), ImVec2(p.x + total, p.y + ui::Dp(22.0f)), ui::kStroke);
        ImGui::Dummy(ImVec2(total, ui::Dp(24.0f)));
    }
    auto block_rows = [&](const char* label, const std::vector<av::Parent>& parents, const std::string& scope) {
        if (parents.empty()) return;
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, ImVec2(p.x + total, p.y + ui::Dp(24.0f)), ui::kBg);
            ui::FontScope fs(ui::FontRole::Small);
            dl->AddText(ImVec2(p.x + ui::Dp(8.0f), p.y + ui::Dp(5.0f)), ui::kMuted, (std::string(label) + kDot + scope).c_str());
            ImGui::Dummy(ImVec2(total, ui::Dp(24.0f)));
        }
        for (const av::Parent& par : parents)
            for (const av::Leaf& l : par.leaves) {
                const av::Target& t = f.targets[static_cast<size_t>(l.block)];
                ImGui::PushID(l.key.c_str());
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float  ty = std::floor(p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f);
                if (ui::SelectRow("##mxrow", g.sel.Has(l.key), 0, rh, 0, ui::kRadiusSm, total)) {
                    g.sel.Clear();
                    g.sel.keys.push_back(l.key);
                    g.sel.anchor = g.sel.cursor = l.key;
                    g.closed[par.id] = false;
                    g.folded[par.is_auto ? "auto" : "rules"] = false;
                    g.overview = false;
                }
                ui::EllipsisTextTip(dl, ImVec2(p.x + ui::Dp(8.0f), ty), p.x + c0 - ui::Dp(6.0f), ui::kText, LeafTitle(par, l));
                {  // the library cell: dot, name, where it comes from
                    float x = p.x + c0 + ui::Dp(8.0f);
                    if (t.lib) {
                        dl->AddCircleFilled(ImVec2(x + ui::Dp(4.0f), ty + ImGui::GetTextLineHeight() * 0.5f), ui::Dp(4.0f), ui::LibDot(t.lib->color));
                        x += ui::Dp(14.0f);
                    }
                    std::string text = t.lib ? t.lib->name : (t.Unset() ? std::string("\xE2\x80\x94") : std::string("None"));
                    if (t.lib_empty) text += "  [empty]";
                    else if (t.lib_blocked) text += "  [blocked]";
                    if (!t.OwnLib() && !t.Unset()) text += "  " + t.ea.from;
                    ui::EllipsisTextTip(dl, ImVec2(x, ty), p.x + c0 + c1 - ui::Dp(6.0f), t.OwnLib() ? ui::kText : ui::kMuted, text);
                }
                for (size_t i = 0; i < groups.size(); ++i) {
                    const av::Cell c = av::OverviewCell(t, groups[i]);
                    const float    x = p.x + c0 + c1 + cg * static_cast<float>(i) + ui::Dp(8.0f);
                    const char*    txt = c.kind == av::CellKind::Value ? c.text.c_str() : c.kind == av::CellKind::NotReached ? "not reached" : "\xE2\x80\x94";
                    const ImU32    col = c.kind == av::CellKind::Value ? (c.inherited ? ui::kMuted : ui::kText) : ui::kFaint;
                    ui::EllipsisTextTip(dl, ImVec2(x, ty), x + cg - ui::Dp(12.0f), col, txt);
                }
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
                ImGui::PopID();
            }
    };
    block_rows("Bone rules", f.tg.rules, f.pool_text);
    block_rows("Auto detection", f.tg.autos, "this item");
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    ui::ToneText("Grey = inherited (from the character, the project, the item or the Switch default). Click a row to edit it in the tree.",
                 ui::kMuted, true);
    ui::EndPane();
}

// ---- The window --------------------------------------------------------------------------------------------

ImVec2 ClampPos(ImVec2 pos, ImVec2 size)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float          keep = ui::Dp(60.0f);
    pos.x = std::max(vp->Pos.x - size.x + keep, std::min(pos.x, vp->Pos.x + vp->Size.x - keep));
    pos.y = std::max(vp->Pos.y, std::min(pos.y, vp->Pos.y + vp->Size.y - ui::Dp(30.0f)));
    return pos;
}

void RestoreRect()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float          s = ui::UiScale();
    const float          w = std::min(ui::Dp(LoadPrefFloat(kPrefW, kDefaultW)), vp->Size.x - ui::Dp(16.0f));
    const float          h = std::min(ui::Dp(LoadPrefFloat(kPrefH, kDefaultH)), vp->Size.y - ui::Dp(16.0f));
    const float          dx = LoadPrefFloat(kPrefX, -100000.0f), dy = LoadPrefFloat(kPrefY, -100000.0f);
    g.size = ImVec2(std::max(ui::Dp(kMinW), w), std::max(ui::Dp(kMinH), h));
    if (dx > -99999.0f && dy > -99999.0f) g.pos = ImVec2(vp->Pos.x + dx * s, vp->Pos.y + dy * s);
    else g.pos = ImVec2(vp->Pos.x + std::max(ui::Dp(8.0f), vp->Size.x - g.size.x - ui::Dp(380.0f)), vp->Pos.y + ui::Dp(40.0f));
    g.pos = ClampPos(g.pos, g.size);
    g.saved_pos = g.pos;
    g.saved_size = g.size;
    g.rect_known = true;
}

void SaveRectIfChanged()
{
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (std::fabs(g.pos.x - g.saved_pos.x) < 1.0f && std::fabs(g.pos.y - g.saved_pos.y) < 1.0f &&
        std::fabs(g.size.x - g.saved_size.x) < 1.0f && std::fabs(g.size.y - g.saved_size.y) < 1.0f)
        return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float          s = std::max(0.01f, ui::UiScale());
    SavePrefFloat(kPrefX, (g.pos.x - vp->Pos.x) / s);
    SavePrefFloat(kPrefY, (g.pos.y - vp->Pos.y) / s);
    SavePrefFloat(kPrefW, g.size.x / s);
    SavePrefFloat(kPrefH, g.size.y / s);
    g.saved_pos = g.pos;
    g.saved_size = g.size;
}

void DrawTitleRow(const Frame& f)
{
    const float  h = ui::Dp(28.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  close_w = ui::ControlH();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##assigntitle", ImVec2(std::max(1.0f, w - close_w - ui::Dp(4.0f)), h));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const ImVec2 np = ClampPos(ImVec2(g.pos.x + ImGui::GetIO().MouseDelta.x, g.pos.y + ImGui::GetIO().MouseDelta.y), g.size);
        ImGui::SetWindowPos(np);
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    const float ty = std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
    std::string title = "Sound assignment";
    const float tw = ImGui::CalcTextSize(title.c_str()).x;
    dl->AddText(ImVec2(p.x + ui::Dp(4.0f), ty), ui::kText, title.c_str());
    if (!f.item_name.empty()) {
        const float x = p.x + ui::Dp(4.0f) + tw + ui::Dp(6.0f);
        ui::EllipsisTextTip(dl, ImVec2(x, ty), p.x + w - close_w - ui::Dp(8.0f), ui::kMuted, std::string("\xC2\xB7 ") + f.item_name);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - close_w, p.y + (h - close_w) * 0.5f));
    if (ui::IconOnlyButton("##assignclose", ToolIcon(kToolCross), "Close (Esc)")) g.open = false;
    dl->AddLine(ImVec2(p.x - ui::Dp(8.0f), p.y + h), ImVec2(p.x + w + ui::Dp(8.0f), p.y + h), ui::kStroke);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ui::Dp(4.0f)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// "Reads: Kit > Project > Hero > Hero_Run_01 > rule > event": the level being edited lit, the ones beyond it off.
void DrawCrumb(const Frame& f)
{
    struct Lv {
        std::string t;
        bool        cur, off;
    };
    const std::string ch = f.char_name.empty() ? std::string("Character") : f.char_name;
    const std::vector<Lv> lv = {{"Kit", false, false},
                                {"Project", g.scope == kScopeProject, false},
                                {g.scope == kScopeProject ? std::string("Character") : ch, g.scope == kScopeCharacter, g.scope == kScopeProject},
                                {g.scope == kScopeItem && !f.item_name.empty() ? f.item_name : std::string("Item"), g.scope == kScopeItem,
                                 g.scope != kScopeItem},
                                {"rule", false, g.scope != kScopeItem},
                                {"event", false, g.scope != kScopeItem}};
    ui::FontScope fs(ui::FontRole::Small);
    ImDrawList*   dl = ImGui::GetWindowDrawList();
    const ImVec2  p0 = ImGui::GetCursorScreenPos();
    const float   avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float   lh = ImGui::GetTextLineHeight() + ui::Dp(3.0f);
    float         x = 0.0f, y = 0.0f;
    auto put = [&](const std::string& t, ImU32 col, bool strike) {
        const float w = ImGui::CalcTextSize(t.c_str()).x;
        if (x > 0.0f && x + w > avail) {
            x = 0.0f;
            y += lh;
        }
        dl->AddText(ImVec2(p0.x + x, p0.y + y), col, t.c_str());
        if (strike)
            dl->AddLine(ImVec2(p0.x + x, p0.y + y + ImGui::GetTextLineHeight() * 0.55f),
                        ImVec2(p0.x + x + w, p0.y + y + ImGui::GetTextLineHeight() * 0.55f), ui::kStrokeStrong);
        x += w;
    };
    put("Reads:", ui::kFaint, false);
    x += ui::Dp(5.0f);
    for (size_t i = 0; i < lv.size(); ++i) {
        if (i > 0) {
            put("\xE2\x80\xBA", ui::kFaint, false);
            x += ui::Dp(5.0f);
        }
        put(lv[i].t, lv[i].cur ? ui::kAccentText : lv[i].off ? ui::kFaint : ui::kMuted, lv[i].off);
        x += ui::Dp(5.0f);
    }
    ImGui::Dummy(ImVec2(avail, y + lh));
}

void DrawItemPicker(const Frame& f)
{
    std::string preview = f.item_name.empty() ? std::string("No animation item") : f.item_name;
    if (!f.char_name.empty()) preview += kDot + f.char_name;
    if (!ui::BeginComboStyled("##assignitem", preview.c_str(), ui::Dp(210.0f))) return;
    const std::vector<MediaItem*> items = ProjectRavItems();
    int                           i = 0;
    for (MediaItem* it : items) {
        std::string label = ItemNameOf(it);
        if (label.empty()) label = "(unnamed item)";
        const ItemRigInfo rig = ItemRig(it);
        if (const SoundCharacter* c = CharacterFor(GetSoundStore(nullptr), rig.skeleton, RigModel(rig)))
            label += kDot + c->name;
        const bool cur = it == g.item;
        if (ImGui::Selectable((label + "##item" + std::to_string(i++)).c_str(), cur) && !cur) {
            g.item = it;  // window-local: the sound store is untouched
            g.follow = false;
        }
    }
    if (items.empty()) ImGui::TextUnformatted("No animation item in the project");
    ui::EndComboStyled();
}

void DrawFooter()
{
    const float  h = ImGui::GetTextLineHeight() + ui::Dp(10.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    ui::FontScope fs(ui::FontRole::Small);
    dl->AddLine(ImVec2(p.x - ui::Dp(8.0f), p.y), ImVec2(p.x + w + ui::Dp(8.0f), p.y), ui::kStroke);
    const float ty = std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
    const char  right[] = "Each change is one undo step.";
    const float rw = ImGui::CalcTextSize(right).x;
    const bool  show_right = w > rw + ui::Dp(220.0f);
    if (show_right) dl->AddText(ImVec2(p.x + w - rw - ui::Dp(4.0f), ty), ui::kMuted, right);
    const float lim = p.x + w - (show_right ? rw + ui::Dp(16.0f) : 0.0f);
    float       x = p.x + ui::Dp(4.0f);
    if (g.has_last) {
        dl->AddText(ImVec2(x, ty), ui::kMuted, g.last.prefix.c_str());
        x += ImGui::CalcTextSize(g.last.prefix.c_str()).x;
        ui::AddTextIn(dl, ui::FontCaptionBold(), ImVec2(x, ty), ui::kText, g.last.bold.c_str());
        x += ui::TextSizeIn(ui::FontCaptionBold(), g.last.bold.c_str()).x;
        ui::EllipsisTextTip(dl, ImVec2(x, ty), lim, ui::kMuted, g.last.rest);
    } else {
        ui::EllipsisTextTip(dl, ImVec2(x, ty), lim, ui::kMuted, g.last_note.empty() ? std::string("No change yet.") : g.last_note);
    }
    ImGui::Dummy(ImVec2(w, h));
}

void DrawTreePane(const Frame& f, ImVec2 size)
{
    if (ui::BeginPane("##assigntree", size, ui::kSurface)) {
        const std::vector<Row> rows = BuildRows(f);
        if (rows.empty()) ui::SubText("Nothing matches the filter.");
        DrawTreeRows(f, rows);
        HandleTreeKeys(rows);
    }
    ui::EndPane();
}

void DrawTreeAndDetails(const Frame& f, float body_h)
{
    const float total = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    if (total < ui::Dp(640.0f)) {  // a narrow window: the details go under the tree
        const float th = std::max(ui::Dp(60.0f), body_h * 0.45f);
        DrawTreePane(f, ImVec2(0.0f, th));
        if (ui::BeginPane("##assigndet", ImVec2(0.0f, std::max(ui::Dp(40.0f), body_h - th - ImGui::GetStyle().ItemSpacing.y)), ui::kSurface))
            DrawDetails(f);
        ui::EndPane();
        return;
    }
    if (g.split < 0.0f) g.split = total * 0.4f;
    const float hit = ui::Dp(8.0f);
    g.split = std::max(ui::Dp(240.0f), std::min(g.split, std::max(ui::Dp(240.0f), total - ui::Dp(260.0f) - hit)));
    DrawTreePane(f, ImVec2(g.split, body_h));
    ImGui::SameLine(0.0f, 0.0f);
    const ui::SplitterState sp = ui::SplitterBar("##assignsplit", ImVec2(hit, body_h));
    if (sp.active) g.split += sp.delta;
    if (sp.double_clicked) g.split = total * 0.4f;
    ImGui::SameLine(0.0f, 0.0f);
    if (ui::BeginPane("##assigndet", ImVec2(0.0f, body_h), ui::kSurface)) DrawDetails(f);
    ui::EndPane();
}

// Applies a request from the strip: select those rules' leaves, unfold their section.
void ApplyRequest(const Frame& f)
{
    g.request = false;
    g.scope = kScopeItem;
    g.overview = false;
    g.filter[0] = '\0';
    g.sel.Clear();
    for (int b : g.request_blocks) {
        const std::string key = av::LeafKey(b);
        const av::Parent* p = f.tg.ParentOf(key);
        if (!p) continue;
        g.sel.keys.push_back(key);
        g.closed[p->id] = false;
        g.folded[p->is_auto ? "auto" : "rules"] = false;
        if (g.sel.anchor.empty()) {
            g.sel.anchor = g.sel.cursor = key;
            g.nav = "P:" + p->id;
        }
    }
    g.request_blocks.clear();
    g.nav_shown = true;
    g.nav_scroll = true;
}

}  // namespace

void OpenAssignWindow(MediaItem* item, const std::vector<int>& blocks)
{
    g.open = true;
    g.focus_request = true;
    if (item && !blocks.empty()) {
        g.request = true;
        g.request_item = item;
        g.request_blocks = blocks;
    }
}

void CloseAssignWindow()
{
    g.open = false;
}

bool AssignWindowIsOpen()
{
    return g.open;
}

bool TaggingAssignWindowOpen()
{
    if (!g.open || !ImGui::GetCurrentContext()) return false;
    const int age = ImGui::GetFrameCount() - g.focused_frame;
    return age >= 0 && age <= 1;
}

void DrawAssignWindow(const TaggingModel& m)
{
    if (!g.open || !ImGui::GetCurrentContext()) return;
    if (m.item != g.last_model_item) {  // the Tagging view moved to another item: the window follows it again
        g.last_model_item = m.item;
        g.follow = true;
    }
    if (g.request) {
        g.item = g.request_item;
        g.follow = g.request_item == m.item;
    } else if (g.follow) {
        g.item = m.item;
    }
    if (!g.rect_known) RestoreRect();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(g.pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(g.size, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui::Dp(kMinW), ui::Dp(kMinH)), vp->Size);
    if (g.focus_request) {
        ImGui::SetNextWindowFocus();
        g.focus_request = false;
    }
    const bool shown = ui::BeginMovableCard(kWindowId, ImGuiWindowFlags_NoMove);
    if (shown) {
        const int frame = ImGui::GetFrameCount();
        g.drawn_frame = frame;
        g.pos = ImGui::GetWindowPos();
        g.size = ImGui::GetWindowSize();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused) g.focused_frame = frame;
        Frame      f;
        const bool ok = BuildFrame(g.item, &f);
        if (ok) {
            if (g.sel_item != g.item) {  // another item: its rows are not these rows
                g.sel.Clear();
                g.closed.clear();
                g.nav.clear();
                g.sel_item = g.item;
            }
            if (g.sel_sig != f.tg.signature) {
                g.sel_sig = f.tg.signature;
                if (!g.request) g.sel.Clear();
            }
            if (g.request) ApplyRequest(f);
        }
        if (focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) g.open = false;

        DrawTitleRow(f);
        {  // Scope bar: where you edit, the item, the reading order.
            static const char* const kScopes[] = {"Project", "Character", "Item"};
            int                      sc = g.scope;
            if (ui::Segmented("##assignscope", kScopes, 3, &sc)) {
                g.scope = sc;
                g.sel.Clear();
                g.sel_sig.clear();
            }
            if (g.scope == kScopeItem) {
                ui::SameLineIfFits(ui::Dp(210.0f));
                DrawItemPicker(f);
            }
            DrawCrumb(f);
        }
        ImGui::Separator();
        {  // View, filter, Library view
            static const char* const kModes[] = {"Tree", "Overview"};
            int                      md = g.overview ? 1 : 0;
            if (ui::Segmented("##assignmode", kModes, 2, &md)) g.overview = md == 1;
            ui::SameLineIfFits(ui::Dp(150.0f));
            const char* hint = g.scope == kScopeItem      ? "Filter rules and types"
                               : g.scope == kScopeProject ? "Filter kits and body parts"
                                                          : "Filter characters and body parts";
            const float bw = ui::ButtonWidth("Library view \xE2\x86\x97##assignlib");
            const float fw = std::max(ui::Dp(100.0f), std::min(ui::Dp(240.0f), ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x));
            ui::TextField("##assignfilter", hint, g.filter, sizeof(g.filter), fw);
            if (ui::SameLineIfFits(bw)) {
                const float room = ImGui::GetContentRegionAvail().x - bw;
                if (room > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room);
            }
            if (ui::SolidButton("Library view \xE2\x86\x97##assignlib")) {
                const std::vector<const av::Target*> sel = ok ? SelectedTargets(f) : std::vector<const av::Target*>();
                std::set<std::string>                libs;
                for (const av::Target* t : sel)
                    if (t->lib) libs.insert(t->lib->uid);
                if (libs.size() == 1) OpenLibraryView(sel[0]);
                else SetViewMode(ViewMode::Library);
            }
            ui::ItemTipText("Open the Library view (this window stays open)");
        }
        const float foot_h = ImGui::GetTextLineHeight() + ui::Dp(10.0f) + ImGui::GetStyle().ItemSpacing.y;
        const float body_h = std::max(ui::Dp(80.0f), ImGui::GetContentRegionAvail().y - foot_h);
        if (!ok) {
            ui::SubText("No animation item with readable rules is shown. Select a RAV animation item in REAPER.");
            ImGui::Dummy(ImVec2(0.0f, body_h - ImGui::GetTextLineHeightWithSpacing()));
        } else if (g.scope != kScopeItem) {
            // 13-15c: the Project and Character scopes replace this branch (their tree + details + overview go
            // here, dispatched on g.scope like the Item ones below).
            ui::SubText("Project and Character levels: next update");
            ImGui::Dummy(ImVec2(0.0f, body_h - ImGui::GetTextLineHeightWithSpacing()));
        } else if (g.overview) {
            DrawOverview(f, body_h);
        } else {
            DrawTreeAndDetails(f, body_h);
        }
        DrawFooter();
        SaveRectIfChanged();
    }
    ui::EndMovableCard();
}

std::vector<int> RuleGroupBlocks(const std::vector<Block>& blocks, int b)
{
    std::vector<int> out;
    if (b < 0 || static_cast<size_t>(b) >= blocks.size()) return out;
    const av::Targets tg = av::BuildTargets(blocks);
    if (const av::Parent* p = tg.ParentOf(av::LeafKey(b))) {
        for (const av::Leaf& l : p->leaves) out.push_back(l.block);
    } else {
        out.push_back(b);
    }
    return out;
}

void DrawAssignSummaryRow(const char* id, MediaItem* item, const std::vector<Block>& blocks, const std::vector<int>& open_blocks,
                          const char* label)
{
    if (!item || blocks.empty()) return;
    RefreshCache(item);
    const SoundStore&     store = GetSoundStore(nullptr);
    const ItemRigInfo     rig = ItemRig(item);
    const SoundCharacter* ch = CharacterFor(store, rig.skeleton, RigModel(rig));
    std::vector<av::Target> ts;
    for (const Block& b : blocks) ts.push_back(av::MakeTarget(store, g_cache.rec, ch, b, std::string(), std::string()));
    std::vector<const av::Target*> ptrs;
    for (const av::Target& t : ts) ptrs.push_back(&t);
    const av::Summary sum = av::Summarize(store, ptrs, true);

    ImGui::PushID(id);
    const bool   can = !open_blocks.empty();
    const char*  btn = "Assign...##assign";
    const float  bw = ui::ButtonWidth(btn);
    const float  sp = ImGui::GetStyle().ItemSpacing.x;
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  lw = label && label[0] ? ImGui::CalcTextSize(label).x + ui::Dp(6.0f) : 0.0f;
    const bool   narrow = w < lw + ui::Dp(120.0f) + bw + sp;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  fh = ImGui::GetFrameHeight();
    const float  ty = std::floor(p.y + (fh - ImGui::GetTextLineHeight()) * 0.5f);
    if (lw > 0.0f) dl->AddText(ImVec2(p.x, ty), ui::kMuted, label);
    const float sum_w = std::max(ui::Dp(40.0f), (narrow ? w : w - bw - sp) - lw);
    const bool  cut = PaintSummary(dl, p.x + lw, ty, sum_w, sum, false);
    ImGui::Dummy(ImVec2(sum_w + lw, fh));
    if (cut) ui::ItemTipText(sum.Plain().c_str());
    if (!narrow) ImGui::SameLine(0.0f, sp);
    if (!can) ImGui::BeginDisabled();
    if (ui::SolidButton(btn) && can) OpenAssignWindow(item, open_blocks);
    if (!can) ImGui::EndDisabled();
    ui::ItemTipText(can ? "Open the Sound assignment window on this" : "Detect first, or set it at Project / Character level");
    ImGui::PopID();
}

}  // namespace rav

#endif  // _WIN32
