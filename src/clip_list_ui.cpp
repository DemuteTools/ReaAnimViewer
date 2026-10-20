// SPDX-License-Identifier: MIT
//
// See clip_list_ui.h. Sizes are the mockup's CSS px at the screen's DPI (ui::Dp).

#include "clip_list_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "clip_list_plan.h"
#include "console_log.h"
#include "watchdog.h"  // freeze audit: breadcrumbs and scopes
#include "pcm_source_anim.h"  // ReadPlayCandidate (story 11-7)
#include "ui_fold_all.h"
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
    bool                             search_active = false; // last frame: the search field had the keyboard
    bool                             caret_to_end = false;  // the search, focused for typed characters: no select-all
    char                             search[160] = {};
    MediaItem*                       kb = nullptr;          // the arrows' dashed row
    MediaItem*                       current = nullptr;     // the item RAV shows
    MediaItem*                       scroll_to = nullptr;   // 6-5-10b: a row to bring into view (REAPER's selection)
    // The list as read, and what it was read at.
    std::vector<cliplist::ClipEntry> entries;
    std::vector<RowItem>             items;
    std::unordered_map<MediaItem*, int> index_of;           // item -> its row (IndexOf)
    ReaProject*                      read_proj = nullptr;
    int                              read_count = INT_MIN;
    unsigned                         read_gen = 0;          // one more at every read
    // The list as shown (BuildListView, flattened, each line's top), built again only when the
    // read, the grouping or the search changed: not every frame.
    cliplist::ListView               view;
    std::vector<cliplist::ListLine>  lines;
    std::vector<float>               tops;
    float                            total_h = 0.0f;
    float                            tops_header_h = -1.0f, tops_row_h = -1.0f;
    bool                             view_ok = false;
    ReaProject*                      view_proj = nullptr;
    unsigned                         view_gen = 0;
    int                              view_grouping = -1;
    std::string                      view_search;
    unsigned                         view_fold_gen = 0;
    // 6-5-10b: REAPER's item selection as last seen (its RAV items), what it was read at, and when
    // it is read again at the latest (a selection change may not move the project's state count).
    std::vector<MediaItem*>          sel_before;
    bool                             sel_primed = false;    // false: the next read only records it
    ReaProject*                      sel_proj = nullptr;
    int                              sel_count = INT_MIN;
    double                           sel_next = 0.0;
    bool                             popup_last_frame = false;  // an ImGui popup was open at the last frame's end
    ImGuiWindow*                     panel_window = nullptr;
    ImGuiWindow*                     button_window = nullptr;
    // 6-5-10c: what the log says about a close (the view, the frames and seconds since the last row
    // click: a click jumps, then the item under the cursor loads).
    unsigned long long               frame = 0;              // one more at every DrawClipList
    long long                        click_frame = -1;       // the frame of the last row click (-1: none)
    double                           click_time = 0.0;       // its time (NowSeconds)
    std::string                      view_name;              // the view shown
    // 6-5-10c: a project load REAPER reported for the tab shown (or a load it does not name), decided
    // at the next frame (ProjectLoadVerdict): what the tab showed at the last frame, the items the
    // list showed then, and the grouping and folds the load dropped (back when nothing was opened).
    cliplist::ProjectLook            look;                   // the tab shown, read at every frame
    bool                             load_pending = false;
    cliplist::ProjectLook            load_before;
    // 6-5-10c review: a handful of the tab's items (pointer and GUID), open or closed: a load where
    // most are gone (or another item took the address) opened the same file again.
    std::vector<std::pair<MediaItem*, std::string>> load_listed;
    ReaProject*                      load_proj = nullptr;
    int                              load_grouping = -1;     // -1: the default (no line)
    bool                             load_grouping_read = false;  // the load read the project's line
    cliplist::FoldSet                load_folds;
    // 6-5-10c: the "open, but not shown" warnings, each written once until it no longer holds.
    bool                             warned_narrow = false, warned_short = false, warned_skipped = false;
    bool                             warned_covered = false, warned_alpha = false;
    bool                             warned_draw = false;  // "could not be drawn": once per episode
};
State g;
// 6-5-10c review: the items sampled before a load (CheckProjectLoad tests these, not every item).
constexpr int kLoadSample = 32;
// 6-5-10c: the open state SetClipListOpen last set. Only SetClipListOpen opens or closes the list; a
// frame that finds `g.open` otherwise says so in the log and puts it back (DrawClipList).
bool g_open_wanted = false;

// 6-5-10c: seconds on a steady clock (the log's "s after a row click"; no ImGui context needed).
double NowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 6-5-10c: the note after a close's cause in the log.
std::string CloseNoteNow()
{
    const long long frames = g.click_frame < 0 ? -1 : static_cast<long long>(g.frame) - g.click_frame;
    return cliplist::CloseNote(g.view_name, frames, g.click_frame < 0 ? -1.0 : NowSeconds() - g.click_time);
}

// The grouping of each open project (absent = the default, by track). A project's line is
// dropped when it loads (BeginLoadProjectState) and when its tab closes (PruneTick), so a new
// project that gets a closed one's address starts with the default.
std::map<ReaProject*, int> g_grouping;
bool                       g_timer_on = false;

// 6-5-10b: the folded groups of each open project, for the session (view state: never written in
// the project, no undo point); one more g_fold_gen at every change (the shown list follows).
std::map<ReaProject*, cliplist::FoldSet> g_folds;
unsigned                                 g_fold_gen = 0;

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
        ReaProject* const proj = LoadSaveProject();
        g_grouping[proj] = static_cast<int>(cliplist::GroupingFromInt(std::atoi(line + n + 1)));
        if (g.load_pending && proj == g.load_proj) g.load_grouping_read = true;  // 6-5-10c: the load's own
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
        ReaProject* const named = f_project_in_load_save ? f_project_in_load_save() : nullptr;
        ReaProject* const cur = CurrentProject();
        ReaProject* const proj = named ? named : cur;  // LoadSaveProject()
        // A project (re)opened in the tab shown: the list starts closed with an empty search (Antho,
        // 2026-10-14), and the list stays open until the user closes it otherwise (6-5-10b). REAPER
        // also loads project states that are not the tab shown (another tab, a project read for
        // another purpose): those never touch the list. 6-5-10c: a load REAPER says is the tab shown's,
        // or does not name (File > New may load its template unnamed), no longer closes the list
        // here: REAPER reports more loads than the user's (the list kept vanishing after a row click,
        // 2026-10-14). The next frame closes it only when another project shows once the load is over
        // (CheckProjectLoad); otherwise nothing changed.
        if ((!named || named == cur) && !g.load_pending) {
            g.load_pending = true;
            g.load_proj = proj;
            g.load_before = g.look;  // what the tab showed at the last frame
            g.load_listed.clear();
            if (proj) {
                const int n = CountMediaItems(proj);
                const int k = std::min(n, kLoadSample);
                for (int j = 0; j < k; ++j) {
                    MediaItem* const it = GetMediaItem(proj, static_cast<int>((static_cast<long long>(j) * n) / k));
                    char             guid[64] = {};
                    if (it && GetSetMediaItemInfo_String(it, "GUID", guid, false)) g.load_listed.emplace_back(it, guid);
                }
            }
            const auto gi = g_grouping.find(proj);
            g.load_grouping = gi == g_grouping.end() ? -1 : gi->second;
            const auto fi = g_folds.find(proj);
            g.load_folds = fi == g_folds.end() ? cliplist::FoldSet() : fi->second;
        }
        if (g.load_pending && proj == g.load_proj) g.load_grouping_read = false;  // its own line, if any, follows
        g_grouping.erase(proj);  // the project's own line (if any) follows
        g_folds.erase(proj);
        ++g_fold_gen;
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
    g.index_of.clear();
    const int n = CountMediaItems(proj);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (!it) continue;
        PlayCandidate c;
        std::string   path;
        // Not a RAV animation (or no file). No length refresh: the list shows the item's own
        // position and length, never the file's (no disk stamp check per item).
        if (!ReadPlayCandidate(it, 0, &c, &path, /*refresh_length=*/false)) continue;
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
        // 6-5-10b: its group's fold follows the track (its GUID), through a rename or a move.
        if (tr) {
            char guid[64] = {};
            if (GetSetMediaTrackInfo_String(tr, "GUID", guid, false)) e.track_key = guid;
        }
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
        g.index_of[it] = static_cast<int>(g.items.size());
        g.items.push_back(ri);
    }
    cliplist::NumberCopies(g.entries);
    ++g.read_gen;
}

// True when a search is typed (spaces alone search nothing): it opens every group it keeps.
bool Searching()
{
    for (const char* c = g.search; *c; ++c)
        if (*c != ' ' && *c != '\t') return true;
    return false;
}

cliplist::FoldSet& FoldsOf(ReaProject* proj)
{
    return g_folds[proj];
}

// The list as shown, built again only when the read, the grouping, the search or a fold changed.
const cliplist::ListView& ShownView(ReaProject* proj, cliplist::Grouping gr)
{
    if (!g.view_ok || g.view_proj != proj || g.view_gen != g.read_gen || g.view_grouping != static_cast<int>(gr) ||
        g.view_search != g.search || g.view_fold_gen != g_fold_gen) {
        g.view = cliplist::BuildListView(g.entries, gr, g.search);
        cliplist::ApplyFolds(g.view, FoldsOf(proj), Searching());
        g.view_fold_gen = g_fold_gen;
        g.lines = cliplist::FlattenListView(g.view);
        g.tops_header_h = g.tops_row_h = -1.0f;  // the tops follow (LineTopsFor)
        g.view_ok = true;
        g.view_proj = proj;
        g.view_gen = g.read_gen;
        g.view_grouping = static_cast<int>(gr);
        g.view_search = g.search;
    }
    return g.view;
}

// Each line's top for these heights (they change with the UI scale): built again only then.
void LineTopsFor(float header_h, float row_h)
{
    if (header_h == g.tops_header_h && row_h == g.tops_row_h) return;
    g.tops = cliplist::LineTops(g.lines, header_h, row_h, &g.total_h);
    g.tops_header_h = header_h;
    g.tops_row_h = row_h;
}

// REAPER's timer: the closed project tabs' groupings are forgotten (a new project may get a closed
// one's address), and a list read from a closed project is read again.
void PruneTick()
{
    RAV_QUIET_SCOPE("PruneTick", nullptr);  // freeze audit
    try {
        if (g_grouping.empty() && g_folds.empty() && !g.read_proj) return;
        std::vector<ReaProject*> open;
        for (int i = 0;; ++i) {
            ReaProject* p = EnumProjects(i, nullptr, 0);
            if (!p) break;
            open.push_back(p);
        }
        auto is_open = [&](ReaProject* p) { return std::find(open.begin(), open.end(), p) != open.end(); };
        for (auto it = g_grouping.begin(); it != g_grouping.end();) it = is_open(it->first) ? std::next(it) : g_grouping.erase(it);
        for (auto it = g_folds.begin(); it != g_folds.end();) it = is_open(it->first) ? std::next(it) : g_folds.erase(it);
        if (g.read_proj && !is_open(g.read_proj)) {
            g.read_proj = nullptr;
            g.read_count = INT_MIN;
            g.view_ok = false;
        }
    } catch (...) {
    }
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
    const auto it = g.index_of.find(item);
    return it == g.index_of.end() ? -1 : it->second;
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
    // 6-5-10c: when the click was, for the log of a close that follows it.
    g.click_frame = static_cast<long long>(g.frame);
    g.click_time = NowSeconds();
    // Already the only selected item: the selection stays as it is (no undo point, no redraw).
    const bool alone = CountSelectedMediaItems(proj) == 1 && GetSelectedMediaItem(proj, 0) == it;
    if (!alone && f_select_all && f_set_selected) {
        f_select_all(proj, false);
        f_set_selected(it, true);
        if (UndoIncludesItemSelection()) Undo_OnStateChangeEx("Change media item selection", UNDO_STATE_ITEMS, -1);
        UpdateArrange();
    }
    SetEditCurPos(pos, true, true);
    g.kb = nullptr;
    // 6-5-10b: the selection this click made is not a selection to focus (the row is already here).
    g.sel_before.assign(1, it);
}

// 6-5-10b: REAPER's selection, followed while the list is open. An item newly selected in REAPER
// (the arrange, an action, a script) gets the dashed row (Enter goes there; the arrows go on from
// it), comes into view and opens its folded group; the item RAV shows stays lit. Read when the
// project's state count moves, and at least every 0.2 s (a selection change may not count): a few
// selected items read, a lookup each.
void PollSelection(ReaProject* proj)
{
    const int    count = GetProjectStateChangeCount(proj);
    const double now = ImGui::GetTime();
    if (g.sel_primed && proj == g.sel_proj && count == g.sel_count && now < g.sel_next) return;
    const bool record_only = !g.sel_primed || proj != g.sel_proj;  // the list opened, or another tab
    g.sel_proj = proj;
    g.sel_count = count;
    g.sel_next = now + 0.2;
    g.sel_primed = true;
    std::vector<MediaItem*> sel;
    const int n = std::min(CountSelectedMediaItems(proj), 64);  // the focus needs the first few only
    for (int i = 0; i < n; ++i)
        if (MediaItem* it = GetSelectedMediaItem(proj, i); it && IndexOf(it) >= 0) sel.push_back(it);
    std::vector<int> before, after;
    for (MediaItem* it : g.sel_before) before.push_back(IndexOf(it));
    for (MediaItem* it : sel) after.push_back(IndexOf(it));
    g.sel_before = std::move(sel);
    if (record_only) return;
    const int focus = cliplist::SelectionFocus(before, after);
    if (focus < 0) return;
    MediaItem* const item = g.items[static_cast<size_t>(focus)].item;
    // Its group opens (a fold must not hide what REAPER just selected); a search that hides it stays.
    const std::string key = cliplist::GroupKey(g.entries[static_cast<size_t>(focus)], GroupingOf(proj));
    if (!key.empty() && FoldsOf(proj).erase(key)) ++g_fold_gen;
    g.kb = (item == g.current) ? nullptr : item;  // the item shown is lit already
    g.scroll_to = item;
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

// A row's and a group header's heights (the list's lines; ItemSpacing is 0 there).
float RowHeight()
{
    return ui::Dp(4.0f) * 2.0f + ui::FontBody()->FontSize + ui::FontSmall()->FontSize;
}

float HeaderHeight()
{
    return ui::Dp(8.0f) + ui::FontSmall()->FontSize + ui::Dp(3.0f);
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
    const float                name_h = name_font->FontSize;
    const float                w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float                h = RowHeight();
    ImGui::PushID(idx);
    if (ImGui::InvisibleButton("##row", ImVec2(w, h))) *clicked = idx;
    const bool   hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
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

// The group header: its fold triangle, the track's colour bar, its name in capitals, its count.
// 6-5-10b: a click anywhere on it folds the group to this line, or opens it (`searching`: every
// group the search kept is open, the triangle says so, and a click does nothing).
// Returns true when clicked.
bool DrawGroupHeader(int gi, const cliplist::GroupView& gv, cliplist::Grouping gr, bool searching)
{
    ImFont* const font = ui::FontSmall();
    const float   w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float   top_pad = ui::Dp(8.0f), pad_x = ui::Dp(4.0f);
    const float   h = HeaderHeight();
    const ImVec2  a = ImGui::GetCursorScreenPos();
    ImGui::PushID(gi + 1000000);
    const bool clicked = ImGui::InvisibleButton("##grp", ImVec2(w, h)) && !searching;
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered && !searching)
        ui::ItemTipText(gv.folded ? "Show this group's clips" : "Fold this group (only its title stays)");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = a.y + top_pad;
    if (hovered && !searching)
        dl->AddRectFilled(ImVec2(a.x, a.y + top_pad - ui::Dp(3.0f)), ImVec2(a.x + w, a.y + h), ui::kHover, ui::kRadiusSm);
    const float bar_h = ui::Dp(12.0f), bar_w = ui::Dp(4.0f);
    const float bar_y = y + (font->FontSize - bar_h) * 0.5f;
    float       x = a.x + pad_x;
    {
        const float tri = ui::Dp(10.0f);
        ui::DrawFoldTriangle(dl, ImVec2(x + tri * 0.5f, y + font->FontSize * 0.5f), !gv.folded,
                             hovered && !searching ? ui::kText : ui::kMuted);
        x += tri + ui::Dp(4.0f);
    }
    if (gr == cliplist::Grouping::Track || gr == cliplist::Grouping::File) {
        // The track's colour; a file's bar is the mockup's plain grey (stroke-strong).
        const ImU32 ink = gr == cliplist::Grouping::Track ? TrackInk(gv) : ui::kStrokeStrong;
        dl->AddRectFilled(ImVec2(x, bar_y), ImVec2(x + bar_w, bar_y + bar_h), ink, ui::Dp(2.0f));
        x += bar_w + ui::Dp(6.0f);
    }
    const std::string count = std::to_string(gv.rows.size());
    const float       count_w = ui::TextSizeIn(font, count.c_str()).x;
    const float       right = a.x + w - ui::Dp(8.0f);
    ui::AddTextIn(dl, font, ImVec2(right - count_w, y), ui::kFaint, count.c_str());
    std::string label = gv.label;
    for (char& c : label)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const std::string shown = ui::EllipsizeToWidth(font, label, std::max(0.0f, right - count_w - ui::Dp(6.0f) - x));
    ui::AddTextIn(dl, font, ImVec2(x, y), ui::kMuted, shown.c_str());
    return clicked;
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

// The panel, `w` x `h` at (x, top): 6-5-10b: between the menu button and the bottom-left controls.
void DrawPanel(float x, float top, float w, float h)
{
    RefreshItems();
    ReaProject* const        proj = CurrentProject();
    PollSelection(proj);
    const cliplist::Grouping gr = GroupingOf(proj);
    // The search field's own Esc clears a typed search; an empty one (or a row last clicked)
    // closes the list. Read before the field runs.
    // One Esc, one thing: an Esc that closed an ImGui menu / combo / popup (open at the last
    // frame's end; ImGui closed it before this frame's windows) is not the list's.
    const bool esc = g.has_focus && !g.popup_last_frame && ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    ImGui::SetNextWindowPos(ImVec2(x, top), ImGuiCond_Always);
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
    // 6-5-10c: Dear ImGui skipping or hiding the open panel is written to the log (once while it lasts).
    const bool skipped = !shown || ImGui::GetCurrentWindow()->Hidden;
    if (skipped && !g.warned_skipped) LogWarn("clip list: open, but Dear ImGui did not show its panel [%s]", CloseNoteNow().c_str());
    g.warned_skipped = skipped;
    if (!shown) {
        ImGui::End();
        return;
    }
    g.panel_window = ImGui::GetCurrentWindow();
    ui::WindowShadow(0.6f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Its right edge (stroke-strong), over the view; 6-5-10b: and its top and bottom edges, now that
    // the view shows above and under it.
    const float bottom = top + h;
    dl->AddLine(ImVec2(x + w - 0.5f, top), ImVec2(x + w - 0.5f, bottom), ui::kStrokeStrong);
    dl->AddLine(ImVec2(x, top + 0.5f), ImVec2(x + w, top + 0.5f), ui::kStrokeStrong);
    dl->AddLine(ImVec2(x, bottom - 0.5f), ImVec2(x + w, bottom - 0.5f), ui::kStrokeStrong);

    const cliplist::ListView& lv = ShownView(proj, gr);
    int jump = PanelKeys(lv.order);
    if (esc) {
        const bool typing = ImGui::GetActiveID() != 0 && ImGui::GetIO().WantTextInput;
        if (g.search[0] && !typing) g.search[0] = '\0';  // a row had the keyboard: clear here
        else if (!g.search[0]) SetClipListOpen(false, "Esc in the list", true);  // empty: close (typing: the field clears it)
    }

    // Header: the title and the count (8 18 6 10).
    const float pad_l = ui::Dp(10.0f), pad_r = ui::Dp(18.0f);
    const float inner_w = std::max(1.0f, w - pad_l - pad_r);
    {
        ImFont* const     tf = ui::FontSemibold();
        ImFont* const     nf = ui::FontSmall();
        const std::string count = std::to_string(lv.shown) + " / " + std::to_string(lv.total);
        const float       count_w = ui::TextSizeIn(nf, count.c_str()).x;
        const float       y = top + ui::Dp(8.0f);
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
            // 6-5-10b review: a row had the keyboard and characters were typed (the viewer took their
            // keys, ListTypesKey): they go on the search's end and the field takes the keyboard, so
            // "walk" searches and L does not close the list. Taken from Dear ImGui's queue here, or
            // the field would type them twice once it is active.
            ImGuiIO& io = ImGui::GetIO();
            if (g.has_focus && !g.search_active && !io.KeyCtrl && !io.KeyAlt && io.InputQueueCharacters.Size > 0) {
                size_t len = std::strlen(g.search);
                for (const ImWchar c : io.InputQueueCharacters) {
                    if (c < 0x20 || c == 0x7F) continue;
                    char      utf8[5] = {};
                    ImTextCharToUtf8(utf8, static_cast<unsigned int>(c));
                    const size_t n = std::strlen(utf8);
                    if (n == 0 || len + n >= sizeof(g.search)) continue;
                    std::memcpy(g.search + len, utf8, n);
                    len += n;
                    g.search[len] = '\0';
                    g.kb = nullptr;
                    g.focus_search = true;
                    g.caret_to_end = true;
                }
                io.InputQueueCharacters.resize(0);
            }
            if (g.focus_search) {
                ImGui::SetKeyboardFocusHere();
                g.focus_search = false;
            }
            ui::TextField("##clipsearch", "Search a clip, a file, a track", g.search, sizeof(g.search), 0.0f,
                          ImGuiInputTextFlags_EscapeClearsAll);
            if (ImGui::IsItemEdited()) g.kb = nullptr;
            g.search_active = ImGui::IsItemActive();
            // Focused for typed characters: the caret after them (a keyboard focus selects all, and
            // the next character would replace what was typed).
            if (g.caret_to_end && g.search_active) {
                if (ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetItemID())) st->ReloadUserBufAndMoveToEnd();
                g.caret_to_end = false;
            }
            const float gap = ui::Dp(6.0f) - 2.0f * ImGui::GetStyle().ItemSpacing.y;
            if (gap > 0.0f) ImGui::Dummy(ImVec2(0.0f, gap));
            static const char* const kGroupings[cliplist::kGroupingCount] = {"Track", "File", "None"};
            int sel = static_cast<int>(gr);
            // Kept in the project for its next save (view state: no undo point, no unsaved mark).
            if (ui::Segmented("##clipgrouping", kGroupings, cliplist::kGroupingCount, &sel)) g_grouping[proj] = sel;
            // 6-5-10b: Expand all / Collapse all (13-33's pair), right-aligned after the grouping (under
            // it when narrow); none without groups. View only, for the session.
            if (gr != cliplist::Grouping::None) {
                const float fw = ui::FoldAllButtonsWidth();
                const float seg_right = ImGui::GetItemRectMax().x;
                const float room_right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
                const float sp = ImGui::GetStyle().ItemSpacing.x;
                if (room_right - seg_right - sp >= fw) {
                    ImGui::SameLine(0.0f, 0.0f);  // centred on the segments' well (2 px around them)
                    ImGui::SetCursorScreenPos(ImVec2(room_right - fw, ImGui::GetCursorScreenPos().y + ui::Dp(2.0f)));
                } else {
                    ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, room_right - fw),
                                                     ImGui::GetCursorScreenPos().y));
                }
                const bool searching = Searching();
                cliplist::FoldSet& folds = FoldsOf(proj);
                const int r = ui::FoldAllButtons(
                    "##clipfoldall", cliplist::CanExpandAll(lv, folds, searching), cliplist::CanCollapseAll(lv, folds, searching),
                    searching ? "Off while a search is typed: the search opens every group with a match (clear it to fold)"
                              : nullptr);
                if (r != 0) {
                    cliplist::SetAllFolds(lv, r < 0, &folds);
                    ++g_fold_gen;
                }
            }
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
    const float list_h = std::max(1.0f, bottom - list_top - foot_h);
    int         clicked = -1;
    ImGui::SetCursorScreenPos(ImVec2(x, list_top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(4.0f), ui::Dp(4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    if (ImGui::BeginChild("##cliprows", ImVec2(w - 1.0f, list_h), ImGuiChildFlags_AlwaysUseWindowPadding)) {
        const int cur = IndexOf(g.current);
        const int kb = IndexOf(g.kb);
        if (lv.groups.empty()) {
            ImGui::Dummy(ImVec2(0.0f, ui::Dp(10.0f)));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui::Dp(6.0f));
            const char* msg = g.entries.empty() ? "No animation item in this project." : "No clip matches.";
            ui::ToneText(msg, ui::kFaint, /*wrap=*/true, std::max(1.0f, ImGui::GetContentRegionAvail().x - ui::Dp(6.0f)));
        }
        // Only the lines on screen are drawn (a project with thousands of items stays light); the
        // list keeps its whole height for the scrollbar.
        const float row_h = RowHeight();
        LineTopsFor(HeaderHeight(), row_h);
        const float top0 = ImGui::GetCursorPosY();  // the first line's top, in the child's content
        const float view_h = ImGui::GetWindowHeight();
        const float scroll = ImGui::GetScrollY();
        // The arrows' row, or the current row when the list opens: centred (SetScrollHereY's 0.5).
        // ImGui moves the scroll (kept within the list) at the next frame's Begin, before its rows.
        // 6-5-10b: a row REAPER just selected only comes into view (no scroll when it shows already).
        const int sel_row = IndexOf(g.scroll_to);
        const int target = (g.scroll_to_kb && kb >= 0) ? kb : (g.scroll_to_current && cur >= 0) ? cur : -1;
        if (target < 0 && sel_row >= 0) {
            for (size_t i = 0; i < g.lines.size(); ++i) {
                if (g.lines[i].row != sel_row) continue;
                const float y0 = top0 + g.tops[i], y1 = y0 + row_h;
                if (y0 < scroll || y1 > scroll + view_h)
                    ImGui::SetScrollY(std::max(0.0f, y0 + row_h * 0.5f - view_h * 0.5f));
                break;
            }
        }
        g.scroll_to = nullptr;
        if (target >= 0) {
            for (size_t i = 0; i < g.lines.size(); ++i) {
                if (g.lines[i].row != target) continue;
                ImGui::SetScrollY(std::max(0.0f, top0 + g.tops[i] + row_h * 0.5f - view_h * 0.5f));
                break;
            }
        }
        g.scroll_to_current = false;  // done, or the row is not in the list (filtered out, or none)
        g.scroll_to_kb = false;
        int first = 0, last = 0;
        cliplist::VisibleLines(g.tops, g.total_h, scroll - top0, scroll + view_h - top0, &first, &last);
        const bool searching = Searching();
        std::string fold_click;  // a header clicked: its group folds or opens after the loop
        for (int i = first; i < last; ++i) {
            const cliplist::ListLine& ln = g.lines[static_cast<size_t>(i)];
            ImGui::SetCursorPosY(top0 + g.tops[static_cast<size_t>(i)]);
            if (ln.row < 0) {
                const cliplist::GroupView& gv = lv.groups[static_cast<size_t>(ln.group)];
                if (DrawGroupHeader(ln.group, gv, gr, searching)) fold_click = gv.key;
            } else {
                DrawRow(ln.row, gr, ln.row == cur, ln.row == kb, &clicked);
            }
        }
        ImGui::SetCursorPosY(top0 + g.total_h);
        ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
        if (!fold_click.empty()) {
            cliplist::ToggleFold(fold_click, &FoldsOf(proj));
            ++g_fold_gen;  // the list is built again next frame (lv and the lines stay as drawn here)
            // The dashed row in a group just folded: the arrows go on from the current row.
            const int kb_now = IndexOf(g.kb);
            if (kb_now >= 0 && cliplist::GroupKey(g.entries[static_cast<size_t>(kb_now)], gr) == fold_click) g.kb = nullptr;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    DrawFooter(ImVec2(x, bottom - foot_h), w - 1.0f);

    g.has_focus = g.open && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    if (clicked >= 0) jump = clicked;
    if (jump >= 0) JumpTo(jump);
}

// 6-5-10c: what the tab shows now (ProjectLoadVerdict compares it with what it showed before a load).
cliplist::ProjectLook LookNow(ReaProject** out_proj)
{
    cliplist::ProjectLook l;
    char                  path[4096] = {};
    ReaProject* const     proj = EnumProjects(-1, path, static_cast<int>(sizeof(path)));
    path[sizeof(path) - 1] = '\0';
    l.proj = proj;
    l.path = path;
    l.items = proj ? CountMediaItems(proj) : 0;
    if (out_proj) *out_proj = proj;
    return l;
}

// 6-5-10c: the project load REAPER reported (BeginLoadProjectState), once it is over: the list starts
// afresh (closed, empty search) only when another project shows in the tab; otherwise nothing was
// opened, and the grouping and folds the load dropped come back. Either way the log says so while the
// list is open (a load REAPER reports right after a row click is the first suspect of a vanishing list).
void CheckProjectLoad(const cliplist::ProjectLook& now, ReaProject* proj)
{
    if (!g.load_pending) return;
    g.load_pending = false;
    int still = 0;
    for (const auto& li : g.load_listed) {
        char guid[64] = {};
        if (li.first && proj && ValidatePtr2(proj, li.first, "MediaItem*") &&
            GetSetMediaItemInfo_String(li.first, "GUID", guid, false) && li.second == guid)
            ++still;
    }
    const cliplist::LoadVerdict v =
        cliplist::ProjectLoadVerdict(g.load_before, now, static_cast<int>(g.load_listed.size()), still);
    g.load_listed.clear();
    if (v == cliplist::LoadVerdict::Stays) {
        if (!g.load_grouping_read && g.load_grouping >= 0) g_grouping[g.load_proj] = g.load_grouping;
        if (!g.load_folds.empty()) {
            g_folds[g.load_proj] = g.load_folds;
            ++g_fold_gen;
        }
        g.load_folds.clear();
        if (g.open)
            LogWarn("clip list: REAPER reloaded the project state of the tab shown, but no other project shows "
                    "(same file, same items): the list stays open [%s]",
                    CloseNoteNow().c_str());
        return;
    }
    g.load_folds.clear();
    g.search[0] = '\0';
    g.kb = nullptr;
    g.scroll_to = nullptr;
    g.sel_primed = false;
    const std::string why = std::string("a project was opened in this tab: ") + cliplist::LoadVerdictText(v);
    SetClipListOpen(false, why.c_str());
}

// 6-5-10c: the open panel is checked once the frame's windows are in their final order: shown, and
// under no other viewer window (only its button, the open menu, popups and tooltips may be above it).
// Each finding is written once while it lasts.
void CheckPanelShown(ImGuiWindow* menu_open)
{
    ImGuiWindow* const p = g.panel_window;
    if (!g.open || !p) {
        g.warned_covered = false;
        return;
    }
    ImGuiContext&     c = *ImGui::GetCurrentContext();
    const char*       cover = nullptr;
    const float       px0 = p->Pos.x, py0 = p->Pos.y, px1 = p->Pos.x + p->Size.x, py1 = p->Pos.y + p->Size.y;
    bool              above = false;
    for (ImGuiWindow* w : c.Windows) {
        if (w == p) {
            above = true;
            continue;
        }
        if (!above || !w->Active || w->Hidden || (w->Flags & ImGuiWindowFlags_ChildWindow)) continue;
        if (w == g.button_window || w == menu_open) continue;
        if (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) continue;
        const float wx0 = w->Pos.x, wy0 = w->Pos.y, wx1 = w->Pos.x + w->Size.x, wy1 = w->Pos.y + w->Size.y;
        if (wx1 <= px0 || wx0 >= px1 || wy1 <= py0 || wy0 >= py1) continue;
        cover = w->Name;
        break;
    }
    const bool gone = !p->Active || p->Hidden || px1 <= 0.0f || py1 <= 0.0f || px1 - px0 < 2.0f || py1 - py0 < 2.0f;
    const bool bad = gone || cover;
    if (bad && !g.warned_covered) {
        if (cover) LogWarn("clip list: open, but its panel is under another window (%s) [%s]", cover, CloseNoteNow().c_str());
        else LogWarn("clip list: open, but its panel is not on screen (%.0f, %.0f  %.0f x %.0f) [%s]", px0, py0, px1 - px0,
                     py1 - py0, CloseNoteNow().c_str());
    }
    g.warned_covered = bad;
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
    if (g_register) g_timer_on = g_register("timer", reinterpret_cast<void*>(&PruneTick)) != 0;
}

void QuitClipList()
{
    if (g_register && g_pcreg_on) g_register("-projectconfig", &g_pcreg);
    g_pcreg_on = false;
    if (g_register && g_timer_on) g_register("-timer", reinterpret_cast<void*>(&PruneTick));
    g_timer_on = false;
    g_grouping.clear();
    g_folds.clear();
    g.entries.clear();
    g.items.clear();
    g.index_of.clear();
    g.sel_before.clear();
    g.view = cliplist::ListView();
    g.lines.clear();
    g.tops.clear();
    g.view_ok = false;
    g.load_pending = false;  // 6-5-10c
    g.load_listed.clear();
    g.load_folds.clear();
}

bool ClipListOpen()
{
    return g.open;
}

void SetClipListOpen(bool open, const char* reason, bool gesture)
{
    g_open_wanted = open;
    if (open == g.open) return;
    // 6-5-10c: a close is kept in the "Copy error log" ring (LogWarn: a release build writes no
    // info line), with the view and how soon after a row click it came: a list that vanishes says why.
    const char* why = reason && reason[0] ? reason : "?";
    // 6-5-10c review: a user's gesture (round button, L, Esc) is an info line, so the ring's 64 lines
    // keep the closes nobody asked for (a hook, an unknown cause) with their timing.
    if (open || gesture) LogInfo("clip list: %s (%s)", open ? "opened" : "closed", why);
    else LogWarn("clip list: closed (%s) [%s]", why, CloseNoteNow().c_str());
    g.open = open;
    g.search_active = false;
    g.caret_to_end = false;
    g.kb = nullptr;
    g.scroll_to = nullptr;
    g.has_focus = false;
    g.sel_primed = false;  // 6-5-10b: opened, the selection then is recorded, not focused
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

bool DrawClipList(float view_w, float menu_bottom, float ui_top, float view_bottom, const char* key, const char* view)
{
    g.panel_window = nullptr;
    g.button_window = nullptr;
    ++g.frame;
    g.view_name = view ? view : "";
    // 6-5-10c: only SetClipListOpen opens or closes the list; anything else that changed it (no such
    // path is known) is written to the log and undone.
    if (g.open != g_open_wanted) {
        LogWarn("clip list: found %s without being %s; %s again [%s]", g.open ? "open" : "closed",
                g.open ? "opened" : "closed", g_open_wanted ? "opened" : "closed", CloseNoteNow().c_str());
        g.open = g_open_wanted;
    }
    // 6-5-10c: what the tab shows, then the verdict on a project load REAPER reported since the last
    // frame (before the panel reads its grouping and folds).
    {
        ReaProject*                 proj = nullptr;
        const cliplist::ProjectLook now = LookNow(&proj);
        CheckProjectLoad(now, proj);
        g.look = now;
    }
    const bool  was_open = g.open;
    const float btn = ui::Dp(22.0f);
    float       panel_w = 0.0f;
    // 6-5-10c: drawn opaque whatever an earlier window left in the style (a leftover alpha is logged).
    {
        const bool see_through = ImGui::GetStyle().Alpha < 0.99f || ImGui::GetCurrentContext()->DisabledStackSize > 0;
        if (g.open && see_through && !g.warned_alpha)
            LogWarn("clip list: an earlier window left the style see-through or disabled (alpha %.2f) [%s]",
                    ImGui::GetStyle().Alpha, CloseNoteNow().c_str());
        g.warned_alpha = g.open && see_through;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
    if (g.open) {
        panel_w = cliplist::PanelWidth(view_w, ui::UiScale());
        // 6-5-10c: an open list with no room for its panel says so in the log (once while it lasts).
        const bool narrow = panel_w < 1.0f;
        if (narrow && !g.warned_narrow)
            LogWarn("clip list: open, but the view (%.0f px wide) has no room for its panel [%s]", view_w,
                    CloseNoteNow().c_str());
        g.warned_narrow = narrow;
        if (panel_w >= 1.0f) {
            // A throw between the panel's Begin and End (std::bad_alloc: it throws nothing of its
            // own) would leave Dear ImGui's window and style stacks unbalanced: its own recovery
            // ends every window and pops every style the panel had begun, so the frame goes on.
            ImGuiErrorRecoveryState before;
            ImGui::ErrorRecoveryStoreState(&before);
            try {
                const cliplist::PanelSpan span =
                    cliplist::PanelVerticalSpan(menu_bottom, ui_top, view_bottom, ui::UiScale());
                // 6-5-10c: and with too little height to show its rows.
                const bool low = span.bottom - span.top < ui::Dp(60.0f);
                if (low && !g.warned_short)
                    LogWarn("clip list: open, but its panel is only %.0f px high (the view's bottom at %.0f px) [%s]",
                            span.bottom - span.top, view_bottom, CloseNoteNow().c_str());
                g.warned_short = low;
                DrawPanel(0.0f, span.top, panel_w, std::max(1.0f, span.bottom - span.top));
                g.warned_draw = false;
            } catch (...) {
                ImGuiIO&   io = ImGui::GetIO();
                const bool was_assert = io.ConfigErrorRecoveryEnableAssert;
                const bool was_tip = io.ConfigErrorRecoveryEnableTooltip;
                io.ConfigErrorRecoveryEnableAssert = false;  // expected here: no assert, no error tooltip
                io.ConfigErrorRecoveryEnableTooltip = false;
                ImGui::ErrorRecoveryTryToRecoverState(&before);
                io.ConfigErrorRecoveryEnableAssert = was_assert;
                io.ConfigErrorRecoveryEnableTooltip = was_tip;
                g.panel_window = nullptr;
                if (!g.warned_draw) LogWarn("clip list: the panel could not be drawn (the frame went on)");
                g.warned_draw = true;
            }
        }
    } else {
        g.has_focus = false;
        g.warned_narrow = g.warned_short = g.warned_skipped = false;
    }
    // Closed: 6 px from the window's left edge; open: 6-5-10b review: wholly outside the panel, 2 px
    // right of its edge (centred on it, it covered the rows' right end and the scrollbar, and a
    // click there closed the list).
    const float cx = g.open ? cliplist::OpenButtonCentreX(panel_w, btn, ui::UiScale()) : ui::Dp(6.0f) + btn * 0.5f;
    // Never over the Model / Skeleton switch: in a window too short for it, no button (the
    // shortcut and Esc still open and close the list).
    if (cliplist::ButtonFits(menu_bottom, ui_top, btn, ui::UiScale())) {
        const float cy = cliplist::ButtonCentreY(menu_bottom, ui_top, btn, ui::UiScale());
        if (RoundButton(ImVec2(cx, cy), key)) SetClipListOpen(!g.open, "the round button", true);
    }
    ImGui::PopStyleVar();  // Alpha
    g.popup_last_frame = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    return g.open && !was_open;
}

void ClipListAfterFrame(ImGuiWindow* menu_open)
{
    CheckPanelShown(menu_open);
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
    return vk == VK_UP || vk == VK_DOWN || vk == VK_RETURN || vk == VK_ESCAPE || cliplist::ListTypesKey(vk, ctrl, alt);
}

}  // namespace rav

#endif  // _WIN32
