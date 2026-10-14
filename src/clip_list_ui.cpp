// SPDX-License-Identifier: MIT
//
// See clip_list_ui.h. Sizes are the mockup's CSS px at the screen's DPI (ui::Dp).

#include "clip_list_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "clip_list_plan.h"
#include "console_log.h"
#include "pcm_source_anim.h"  // ReadPlayCandidate (story 11-7)
#include "ui_theme.h"

namespace rav {
namespace {

using GetCurrentProjectInLoadSaveFn = ReaProject* (*)();
using SelectAllMediaItemsFn = void (*)(ReaProject*, bool);
using SetMediaItemSelectedFn = void (*)(MediaItem*, bool);
using GetConfigVarFn = void* (*)(const char*, int*);

GetCurrentProjectInLoadSaveFn f_project_in_load_save = nullptr;
SelectAllMediaItemsFn         f_select_all = nullptr;
SetMediaItemSelectedFn        f_set_selected = nullptr;
GetConfigVarFn                f_get_config_var = nullptr;
int (*g_register)(const char*, void*) = nullptr;

// One row's REAPER side, beside its plan entry (same index).
struct RowItem {
    MediaItem* item = nullptr;
};

struct State {
    bool                             open = false;
    bool                             focus_search = false;  // the search takes the keyboard next frame
    bool                             scroll_to_current = false;
    bool                             scroll_to_kb = false;
    bool                             has_focus = false;     // last frame: the panel had the keyboard
    char                             search[160] = {};
    MediaItem*                       kb = nullptr;          // the arrows' dashed row
    MediaItem*                       current = nullptr;     // the item RAV shows
    // The list as read, and what it was read at.
    std::vector<cliplist::ClipEntry> entries;
    std::vector<RowItem>             items;
    ReaProject*                      read_proj = nullptr;
    int                              read_count = INT_MIN;
    ImGuiWindow*                     panel_window = nullptr;
    ImGuiWindow*                     button_window = nullptr;
};
State g;

// The grouping of each open project (absent = the default, by track).
std::map<ReaProject*, int> g_grouping;

ReaProject* CurrentProject()
{
    return EnumProjects(-1, nullptr, 0);
}

cliplist::Grouping GroupingOf(ReaProject* proj)
{
    const auto it = g_grouping.find(proj);
    return it == g_grouping.end() ? cliplist::Grouping::Track : cliplist::GroupingFromInt(it->second);
}

// ---- projectconfig: "RAVCLIPLIST <grouping>" ---------------------------------------------------

constexpr char kLineKey[] = "RAVCLIPLIST";

ReaProject* LoadSaveProject()
{
    ReaProject* p = f_project_in_load_save ? f_project_in_load_save() : nullptr;
    return p ? p : CurrentProject();
}

bool ProcessExtensionLine(const char* line, ProjectStateContext*, bool is_undo, project_config_extension_t*)
{
    try {
        if (!line) return false;
        const size_t n = std::strlen(kLineKey);
        if (std::strncmp(line, kLineKey, n) != 0 || (line[n] != ' ' && line[n] != '\t')) return false;
        if (is_undo) return true;  // never written in an undo state; ours all the same
        g_grouping[LoadSaveProject()] = static_cast<int>(cliplist::GroupingFromInt(std::atoi(line + n + 1)));
        return true;
    } catch (...) {
        return true;
    }
}

void SaveExtensionConfig(ProjectStateContext* ctx, bool is_undo, project_config_extension_t*)
{
    try {
        // View state: not part of an undo state (an undo never changes the grouping), and the
        // default writes nothing (a project that never changed it stays as it was).
        if (!ctx || is_undo) return;
        const cliplist::Grouping gr = GroupingOf(LoadSaveProject());
        if (gr == cliplist::Grouping::Track) return;
        ctx->AddLine("%s %d", kLineKey, static_cast<int>(gr));
    } catch (...) {
    }
}

void BeginLoadProjectState(bool is_undo, project_config_extension_t*)
{
    try {
        if (is_undo) return;
        ReaProject* proj = LoadSaveProject();
        g_grouping.erase(proj);  // the project's own line (if any) follows
        // A project (re)opened: the panel starts closed with an empty search (Antho, 2026-10-14).
        g.open = false;
        g.search[0] = '\0';
        g.kb = nullptr;
        g.read_count = INT_MIN;
    } catch (...) {
    }
}

project_config_extension_t g_pcreg = {&ProcessExtensionLine, &SaveExtensionConfig, &BeginLoadProjectState, nullptr};
bool                       g_pcreg_on = false;

// ---- reading the project's RAV items -----------------------------------------------------------

std::string BaseName(const std::string& path)
{
    const size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? path : path.substr(s + 1);
}

void ReadItems(ReaProject* proj)
{
    g.entries.clear();
    g.items.clear();
    const int n = CountMediaItems(proj);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (!it) continue;
        PlayCandidate c;
        std::string   path;
        if (!ReadPlayCandidate(it, 0, &c, &path)) continue;  // not a RAV animation (or no file)
        cliplist::ClipEntry e;
        e.path = path;
        e.file = BaseName(path);
        MediaItem_Take* tk = GetActiveTake(it);
        const char*     tn = tk ? GetTakeName(tk) : nullptr;
        e.clip = (tn && tn[0]) ? tn : e.file;
        e.pos = c.pos;
        e.len = c.len;
        MediaTrack* tr = GetMediaItem_Track(it);
        e.track_num = tr ? static_cast<int>(GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER")) : 0;
        if (e.track_num <= 0) e.track_num = INT_MAX;
        char name[512] = {};
        if (tr) GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
        e.track = name[0] ? std::string(name)
                          : (e.track_num != INT_MAX ? "Track " + std::to_string(e.track_num) : std::string("Track"));
        // REAPER's custom colour: the native colour (0x00BBGGRR on Windows) with 0x1000000 set.
        const int col = tr ? static_cast<int>(GetMediaTrackInfo_Value(tr, "I_CUSTOMCOLOR")) : 0;
        if (col & 0x1000000) {
            e.has_color = true;
            e.track_rgb = (static_cast<unsigned>(col & 0xFF) << 16) | (static_cast<unsigned>(col & 0xFF00)) |
                          (static_cast<unsigned>(col >> 16) & 0xFF);
        }
        // Greyed: the item, its track, or its lane does not play.
        e.muted = c.muted || !c.lane_plays || (tr && GetMediaTrackInfo_Value(tr, "B_MUTE") != 0.0);
        g.entries.push_back(std::move(e));
        RowItem ri;
        ri.item = it;
        g.items.push_back(ri);
    }
    cliplist::NumberCopies(g.entries);
}

// Read again when the project changed (REAPER's state count) or another project tab is current.
void RefreshItems()
{
    ReaProject* proj = CurrentProject();
    const int   count = GetProjectStateChangeCount(proj);
    if (proj == g.read_proj && count == g.read_count) return;
    g.read_proj = proj;
    g.read_count = count;
    ReadItems(proj);
}

int IndexOf(MediaItem* item)
{
    if (!item) return -1;
    for (size_t i = 0; i < g.items.size(); ++i)
        if (g.items[i].item == item) return static_cast<int>(i);
    return -1;
}

// REAPER's "Include selection: item" undo preference (undomask bit 1).
bool UndoIncludesItemSelection()
{
    if (!f_get_config_var) return false;
    int   sz = 0;
    void* p = f_get_config_var("undomask", &sz);
    return p && sz == static_cast<int>(sizeof(int)) && (*static_cast<int*>(p) & 1) != 0;
}

// The jump: the edit cursor to the item's start (the view follows, playback too), the item
// selected alone, as a click on it in the arrange would. Like REAPER, the selection makes an undo
// point only when REAPER's preferences include the item selection in undo.
void JumpTo(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(g.items.size())) return;
    ReaProject* proj = CurrentProject();
    MediaItem*  it = g.items[static_cast<size_t>(idx)].item;
    if (!it || !ValidatePtr2(proj, it, "MediaItem*")) {
        g.read_count = INT_MIN;  // gone since the read: read again
        return;
    }
    const double pos = GetMediaItemInfo_Value(it, "D_POSITION");
    if (f_select_all && f_set_selected) {
        f_select_all(proj, false);
        f_set_selected(it, true);
        if (UndoIncludesItemSelection()) Undo_OnStateChangeEx("Change media item selection", UNDO_STATE_ITEMS, -1);
        UpdateArrange();
    }
    SetEditCurPos(pos, true, true);
    g.kb = nullptr;
}

// ---- drawing -----------------------------------------------------------------------------------

ImU32 TrackInk(const cliplist::GroupView& gv)
{
    if (!gv.has_color) return ui::kStrokeStrong;
    return IM_COL32((gv.rgb >> 16) & 0xFF, (gv.rgb >> 8) & 0xFF, gv.rgb & 0xFF, 0xFF);
}

// A thin chevron, centred on c, `s` px high: pointing right (open the list) or left (close it).
void DrawChevron(ImDrawList* dl, ImVec2 c, float s, bool left, ImU32 col)
{
    const float h = s * 0.5f, w = s * 0.28f;
    const float d = left ? -1.0f : 1.0f;
    // Centred on its own width (the tip a little off-centre reads as centred).
    const float x0 = c.x - d * w * 0.5f, x1 = c.x + d * w * 0.5f;
    dl->PathLineTo(ImVec2(x0, c.y - h));
    dl->PathLineTo(ImVec2(x1, c.y));
    dl->PathLineTo(ImVec2(x0, c.y + h));
    dl->PathStroke(col, ImDrawFlags_None, std::max(1.0f, ui::Dp(1.5f)));
}

// The round button (22 px): discreet over the view (kRaised at .72, muted chevron), kHover with an
// accent-line edge and a bright chevron under the mouse, raised and bright while the list is open.
bool RoundButton(ImVec2 centre, const char* key)
{
    const float d = ui::Dp(22.0f);
    bool        pressed = false;
    if (ui::BeginOverlay("##cliplistbtn", ImVec2(centre.x - d * 0.5f, centre.y - d * 0.5f))) {
        g.button_window = ImGui::GetCurrentWindow();
        pressed = ImGui::InvisibleButton("##cliplisttab", ImVec2(d, d));
        const bool  hovered = ImGui::IsItemHovered();
        const bool  held = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(std::floor(centre.x) + 0.5f, std::floor(centre.y) + 0.5f);
        const float  r = d * 0.5f - 0.5f;
        const ImU32  fill = held ? ui::kFrameHover : hovered ? ui::kHover : g.open ? ui::kRaised : ui::WithAlpha(ui::kRaised, 0xB8);
        const ImU32  edge = (hovered || held) ? ui::kAccentLine : ui::kStrokeStrong;
        dl->AddCircleFilled(c, r, fill, 32);
        dl->AddCircle(c, r, edge, 32, 1.0f);
        DrawChevron(dl, c, ui::Dp(9.0f), g.open, (hovered || held || g.open) ? ui::kText : ui::kMuted);
        if (hovered) {
            const bool        has_key = key && key[0];
            const std::string tip = std::string(g.open ? "Close the clip list" : "Project clips") +
                                    (has_key ? std::string(" (") + key + ")" : std::string());
            ui::ItemTipText(tip.c_str());
        }
    }
    ui::EndOverlay();
    return pressed;
}

// A row: the clip's name (and its copy badge), its position on the right, the file (or track) and
// its length under it. Lit when it is the item RAV shows; dashed when the arrows are on it; greyed
// when it does not play.
void DrawRow(int idx, cliplist::Grouping gr, bool current, bool kb, int* clicked)
{
    const cliplist::ClipEntry& e = g.entries[static_cast<size_t>(idx)];
    ImFont* const              name_font = ui::FontBody();
    ImFont* const              small_font = ui::FontSmall();
    ImFont* const              mono_font = ui::FontMonoSmall();
    const float                pad_x = ui::Dp(8.0f), pad_y = ui::Dp(4.0f);
    const float                name_h = name_font->FontSize, sub_h = small_font->FontSize;
    const float                w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float                h = pad_y * 2.0f + name_h + sub_h;
    ImGui::PushID(idx);
    if (ImGui::InvisibleButton("##row", ImVec2(w, h))) *clicked = idx;
    const bool   hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (kb && g.scroll_to_kb) {
        ImGui::SetScrollHereY(0.5f);
        g.scroll_to_kb = false;
    }
    if (current && g.scroll_to_current) {
        ImGui::SetScrollHereY(0.5f);
        g.scroll_to_current = false;
    }
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    unsigned    bits = 0;
    if (hovered) bits |= ui::kRowHovered | ui::kRowHoverStrong;
    if (current) bits |= ui::kRowLit;
    ui::RowBackground(dl, a, b, bits);
    if (kb) ui::DashedRect(dl, a, b, ui::kAccentLine);

    const ImU32 name_ink = e.muted ? ui::kFaint : current ? ui::kAccentText : ui::kText;
    const ImU32 pos_ink = e.muted ? ui::kFaint : ui::kMuted;
    // The position is never cut: the name gets what is left.
    const std::string pos = cliplist::FormatPosition(e.pos);
    const float       pos_w = ui::TextSizeIn(mono_font, pos.c_str()).x;
    const float       x0 = a.x + pad_x, x1 = b.x - pad_x;
    const float       name_right = std::max(x0, x1 - pos_w - ui::Dp(8.0f));
    const float       line1 = a.y + pad_y;
    ui::AddTextIn(dl, mono_font, ImVec2(x1 - pos_w, line1 + (name_h - mono_font->FontSize) * 0.5f), pos_ink, pos.c_str());

    const std::string badge = cliplist::CopyBadge(e);
    ImFont* const     badge_font = ui::FontTiny();
    const float       badge_pad = ui::Dp(6.0f);
    const float       badge_w = badge.empty() ? 0.0f : ui::TextSizeIn(badge_font, badge.c_str()).x + 2.0f * badge_pad;
    const float       name_full = ui::TextSizeIn(name_font, e.clip.c_str()).x;
    // The badge stays whole while the name keeps a few letters; narrower, the badge goes first.
    const bool        show_badge = !badge.empty() && name_right - x0 >= badge_w + ui::Dp(4.0f + 40.0f);
    const float       name_room = std::max(0.0f, name_right - x0 - (show_badge ? badge_w + ui::Dp(4.0f) : 0.0f));
    const std::string name = ui::EllipsizeToWidth(name_font, e.clip, name_room);
    ui::AddTextIn(dl, name_font, ImVec2(x0, line1), name_ink, name.c_str());
    if (show_badge) {
        const float  bx = x0 + std::min(name_full, name_room) + ui::Dp(4.0f);
        const float  bh = badge_font->FontSize + ui::Dp(2.0f);
        const ImVec2 ba(bx, line1 + (name_h - bh) * 0.5f), bb(bx + badge_w, line1 + (name_h + bh) * 0.5f);
        dl->AddRect(ba, bb, ui::kStrokeStrong, bh * 0.5f);
        ui::AddTextIn(dl, badge_font, ImVec2(bx + badge_pad, ba.y + ui::Dp(1.0f)), pos_ink, badge.c_str());
    }
    const std::string sub_full = cliplist::SubLine(e, gr);
    const std::string sub = ui::EllipsizeToWidth(small_font, sub_full, std::max(0.0f, x1 - x0));
    ui::AddTextIn(dl, small_font, ImVec2(x0, line1 + name_h), ui::kFaint, sub.c_str());

    if (hovered && (name != e.clip || sub != sub_full || (!badge.empty() && !show_badge) || e.muted)) {
        std::string tip = e.clip;
        if (!badge.empty()) tip += "  (" + badge + ")";
        tip += "\n" + e.file + "\n" + e.track + "  " + pos + "  " + cliplist::FormatLength(e.len);
        if (e.muted) tip += "\nMuted: it does not play";
        ui::ItemTipText(tip.c_str());
    }
}

// The group header: the track's colour bar, its name in capitals, its count.
void DrawGroupHeader(const cliplist::GroupView& gv, cliplist::Grouping gr)
{
    ImFont* const font = ui::FontSmall();
    const float   w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float   top_pad = ui::Dp(8.0f), bottom_pad = ui::Dp(3.0f), pad_x = ui::Dp(8.0f);
    const float   h = top_pad + font->FontSize + bottom_pad;
    const ImVec2  a = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = a.y + top_pad;
    const float bar_h = ui::Dp(12.0f), bar_w = ui::Dp(4.0f);
    const float bar_y = y + (font->FontSize - bar_h) * 0.5f;
    float       x = a.x + pad_x;
    if (gr == cliplist::Grouping::Track) {
        dl->AddRectFilled(ImVec2(x, bar_y), ImVec2(x + bar_w, bar_y + bar_h), TrackInk(gv), ui::Dp(2.0f));
        x += bar_w + ui::Dp(6.0f);
    }
    const std::string count = std::to_string(gv.rows.size());
    const float       count_w = ui::TextSizeIn(font, count.c_str()).x;
    const float       right = a.x + w - pad_x;
    ui::AddTextIn(dl, font, ImVec2(right - count_w, y), ui::kFaint, count.c_str());
    std::string label = gv.label;
    for (char& c : label)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const std::string shown = ui::EllipsizeToWidth(font, label, std::max(0.0f, right - count_w - ui::Dp(6.0f) - x));
    ui::AddTextIn(dl, font, ImVec2(x, y), ui::kMuted, shown.c_str());
}

// The footer's hints: [up][down] browse, [Enter] go (the second goes under the first when narrow).
float FooterHeight(float w)
{
    const float line = ImGui::GetTextLineHeight() + ui::Dp(3.0f);
    const float one = ImGui::CalcTextSize("\xE2\x86\x91").x + ImGui::CalcTextSize("\xE2\x86\x93").x +
                      ImGui::CalcTextSize(" browse").x + ImGui::CalcTextSize("Enter").x + ImGui::CalcTextSize(" go").x +
                      ui::Dp(8.0f * 3.0f + 4.0f + 8.0f);
    const float inner = w - ui::Dp(10.0f + 18.0f);
    return ui::Dp(12.0f) + (one <= inner ? line : 2.0f * line + ui::Dp(4.0f)) + 1.0f;
}

void DrawFooter(ImVec2 a, float w)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(a.x, a.y + 0.5f), ImVec2(a.x + w, a.y + 0.5f), ui::kStroke);
    const float line = ImGui::GetTextLineHeight() + ui::Dp(3.0f);
    const float left = a.x + ui::Dp(10.0f), right = a.x + w - ui::Dp(18.0f);
    float       x = left, y = a.y + ui::Dp(6.0f) + 1.0f;
    const float text_dy = ui::Dp(1.5f);
    x += ui::KeyCap(dl, ImVec2(x, y), "\xE2\x86\x91", ui::kText);
    x += ui::KeyCap(dl, ImVec2(x, y), "\xE2\x86\x93", ui::kText);
    dl->AddText(ImVec2(x, y + text_dy), ui::kFaint, " browse");
    x += ImGui::CalcTextSize(" browse").x + ui::Dp(8.0f);
    const float second = ImGui::CalcTextSize("Enter").x + ui::Dp(8.0f) + ImGui::CalcTextSize(" go").x;
    if (x + second > right) {
        x = left;
        y += line + ui::Dp(4.0f);
    }
    x += ui::KeyCap(dl, ImVec2(x, y), "Enter", ui::kText);
    dl->AddText(ImVec2(x, y + text_dy), ui::kFaint, " go");
}

// The panel's keys, read from Dear ImGui before the search field takes its own (Esc clears it
// there). Returns the row to jump to, or -1.
int PanelKeys(const std::vector<int>& order)
{
    if (!g.has_focus) return -1;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl || io.KeyAlt) return -1;
    const int cur = IndexOf(g.current);
    const int kb = IndexOf(g.kb);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
        const int delta = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) ? 1 : -1;
        const int next = cliplist::StepRow(order, kb, cur, delta);
        if (next >= 0) {
            g.kb = g.items[static_cast<size_t>(next)].item;
            g.scroll_to_kb = true;
        }
        return -1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        if (kb >= 0) return kb;
        // Typed a search, then Enter: the first row it kept.
        if (g.search[0] && !order.empty()) return order.front();
        return -1;
    }
    return -1;
}

void DrawPanel(float x, float w, float h)
{
    RefreshItems();
    ReaProject* const        proj = CurrentProject();
    const cliplist::Grouping gr = GroupingOf(proj);
    // The search field's own Esc clears a typed search; an empty one (or a row last clicked)
    // closes the list. Read before the field runs.
    const bool esc = g.has_focus && ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    ImGui::SetNextWindowPos(ImVec2(x, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Col(ui::kSurface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    const bool shown = ImGui::Begin("##cliplist", nullptr, kFlags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    if (!shown) {
        ImGui::End();
        return;
    }
    g.panel_window = ImGui::GetCurrentWindow();
    ui::WindowShadow(0.6f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Its right edge (stroke-strong), over the view.
    dl->AddLine(ImVec2(x + w - 0.5f, 0.0f), ImVec2(x + w - 0.5f, h), ui::kStrokeStrong);

    const cliplist::ListView lv = cliplist::BuildListView(g.entries, gr, g.search);
    int jump = PanelKeys(lv.order);
    if (esc) {
        const bool typing = ImGui::GetActiveID() != 0 && ImGui::GetIO().WantTextInput;
        if (g.search[0] && !typing) g.search[0] = '\0';  // a row had the keyboard: clear here
        else if (!g.search[0]) g.open = false;           // empty: close (typing: the field clears it)
    }

    // Header: the title and the count (8 18 6 10).
    const float pad_l = ui::Dp(10.0f), pad_r = ui::Dp(18.0f);
    const float inner_w = std::max(1.0f, w - pad_l - pad_r);
    {
        ImFont* const     tf = ui::FontSemibold();
        ImFont* const     nf = ui::FontSmall();
        const std::string count = std::to_string(lv.shown) + " / " + std::to_string(lv.total);
        const float       count_w = ui::TextSizeIn(nf, count.c_str()).x;
        const float       y = ui::Dp(8.0f);
        const float       title_room = std::max(0.0f, inner_w - count_w - ui::Dp(6.0f));
        const std::string title = ui::EllipsizeToWidth(tf, "Project clips", title_room);
        ui::AddTextIn(dl, tf, ImVec2(x + pad_l, y), ui::kText, title.c_str());
        ui::AddTextIn(dl, nf, ImVec2(x + pad_l + inner_w - count_w, y + (tf->FontSize - nf->FontSize) * 0.5f),
                      ui::kFaint, count.c_str());
        ImGui::SetCursorScreenPos(ImVec2(x + pad_l, y + tf->FontSize + ui::Dp(6.0f)));  // the tools' child
    }

    // Tools: the search, then the grouping on a line of its own, in the header's inner width (a
    // child window, so the field and the segments fit and wrap there).
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        if (ImGui::BeginChild("##cliptools", ImVec2(inner_w, 0.0f), ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            if (g.focus_search) {
                ImGui::SetKeyboardFocusHere();
                g.focus_search = false;
            }
            ui::TextField("##clipsearch", "Search a clip, a file, a track", g.search, sizeof(g.search), 0.0f,
                          ImGuiInputTextFlags_EscapeClearsAll);
            if (ImGui::IsItemEdited()) g.kb = nullptr;
            const float gap = ui::Dp(6.0f) - 2.0f * ImGui::GetStyle().ItemSpacing.y;
            if (gap > 0.0f) ImGui::Dummy(ImVec2(0.0f, gap));
            static const char* const kGroupings[cliplist::kGroupingCount] = {"Track", "File", "None"};
            int sel = static_cast<int>(gr);
            // Kept in the project for its next save (view state: no undo point, no unsaved mark).
            if (ui::Segmented("##clipgrouping", kGroupings, cliplist::kGroupingCount, &sel)) g_grouping[proj] = sel;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        const float sep_y = ImGui::GetItemRectMax().y + ui::Dp(8.0f);
        dl->AddLine(ImVec2(x, sep_y + 0.5f), ImVec2(x + w - 1.0f, sep_y + 0.5f), ui::kStroke);
        ImGui::SetCursorScreenPos(ImVec2(x, sep_y + 1.0f));
    }

    // The rows (4 4 8), scrolling between the tools and the footer.
    const float foot_h = FooterHeight(w);
    const float list_top = ImGui::GetCursorScreenPos().y;
    const float list_h = std::max(1.0f, h - list_top - foot_h);
    int         clicked = -1;
    ImGui::SetCursorScreenPos(ImVec2(x, list_top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(4.0f), ui::Dp(4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    if (ImGui::BeginChild("##cliprows", ImVec2(w - 1.0f, list_h), ImGuiChildFlags_AlwaysUseWindowPadding)) {
        const int cur = IndexOf(g.current);
        const int kb = IndexOf(g.kb);
        if (lv.order.empty()) {
            ImGui::Dummy(ImVec2(0.0f, ui::Dp(10.0f)));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui::Dp(6.0f));
            const char* msg = g.entries.empty() ? "No animation item in this project." : "No clip matches.";
            ui::ToneText(msg, ui::kFaint, /*wrap=*/true, std::max(1.0f, ImGui::GetContentRegionAvail().x - ui::Dp(6.0f)));
        }
        for (const cliplist::GroupView& gv : lv.groups) {
            if (!gv.label.empty()) DrawGroupHeader(gv, gr);
            for (int r : gv.rows) DrawRow(r, gr, r == cur, r == kb, &clicked);
        }
        ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
        g.scroll_to_current = false;  // the current row is not in the list (filtered out, or none)
        g.scroll_to_kb = false;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    DrawFooter(ImVec2(x, h - foot_h), w - 1.0f);

    g.has_focus = g.open && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    if (clicked >= 0) jump = clicked;
    if (jump >= 0) JumpTo(jump);
}

}  // namespace

void InitClipList(reaper_plugin_info_t* rec)
{
    if (!rec) return;
    if (rec->GetFunc) {
        f_project_in_load_save =
            reinterpret_cast<GetCurrentProjectInLoadSaveFn>(rec->GetFunc("GetCurrentProjectInLoadSave"));
        f_select_all = reinterpret_cast<SelectAllMediaItemsFn>(rec->GetFunc("SelectAllMediaItems"));
        f_set_selected = reinterpret_cast<SetMediaItemSelectedFn>(rec->GetFunc("SetMediaItemSelected"));
        f_get_config_var = reinterpret_cast<GetConfigVarFn>(rec->GetFunc("get_config_var"));
    }
    if (!f_select_all || !f_set_selected) LogWarn("clip list: REAPER's item selection functions are missing; a click only moves the cursor");
    g_register = rec->Register;
    if (g_register) g_pcreg_on = g_register("projectconfig", &g_pcreg) != 0;
}

void QuitClipList()
{
    if (g_register && g_pcreg_on) g_register("-projectconfig", &g_pcreg);
    g_pcreg_on = false;
    g_grouping.clear();
    g.entries.clear();
    g.items.clear();
}

bool ClipListOpen()
{
    return g.open;
}

void SetClipListOpen(bool open)
{
    if (open == g.open) return;
    g.open = open;
    g.kb = nullptr;
    g.has_focus = false;
    if (open) {
        g.focus_search = true;  // type at once (the mockup)
        g.scroll_to_current = true;
        g.read_count = INT_MIN;  // read afresh
    }
}

void ClipListSetCurrentItem(MediaItem* item)
{
    g.current = item;
}

bool DrawClipList(float view_w, float view_h, float menu_bottom, float switch_top, const char* key)
{
    g.panel_window = nullptr;
    g.button_window = nullptr;
    const bool  was_open = g.open;
    const float btn = ui::Dp(22.0f);
    float       panel_w = 0.0f;
    if (g.open) {
        panel_w = cliplist::PanelWidth(view_w, ui::UiScale());
        if (panel_w >= 1.0f) {
            try {
                DrawPanel(0.0f, panel_w, std::max(1.0f, view_h));
            } catch (...) {
                LogWarn("clip list: the panel could not be drawn this frame");
            }
        }
    } else {
        g.has_focus = false;
    }
    // Closed: 6 px from the window's left edge; open: centred on the panel's right edge.
    const float cx = g.open ? panel_w : ui::Dp(6.0f) + btn * 0.5f;
    const float cy = cliplist::ButtonCentreY(menu_bottom, switch_top, btn, ui::UiScale());
    if (RoundButton(ImVec2(cx, cy), key)) SetClipListOpen(!g.open);
    return g.open && !was_open;
}

ImGuiWindow* ClipListPanelWindow()
{
    return g.panel_window;
}

ImGuiWindow* ClipListButtonWindow()
{
    return g.button_window;
}

bool ClipListHasFocus()
{
    return g.open && g.has_focus;
}

bool ClipListTakesKey(unsigned vk, bool ctrl, bool /*shift*/, bool alt)
{
    if (!ClipListHasFocus() || ctrl || alt) return false;
    return vk == VK_UP || vk == VK_DOWN || vk == VK_RETURN || vk == VK_ESCAPE;
}

}  // namespace rav

#endif  // _WIN32
