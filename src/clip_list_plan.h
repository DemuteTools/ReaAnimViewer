// SPDX-License-Identifier: MIT
//
// Story 6-5-10 -- the animation clip list (the round button on the left of every view), its pure
// half: the rows' copies, the search, the three groupings and their order, the keyboard's dashed
// row, the texts of a row, and where the panel and its button go at any window size. No REAPER, no
// Windows, no Dear ImGui. The REAPER reading and the drawing are src/clip_list_ui.cpp; the host test
// is tests/clip_list_plan_test.cpp.

#pragma once

#include <set>
#include <string>
#include <vector>

namespace rav {
namespace cliplist {

// One RAV item of the project, as the list shows it.
struct ClipEntry {
    std::string clip;              // the take's name (the clip)
    std::string file;              // its file's name, without the folder
    std::string path;              // its file's full path (copies: the same path and clip)
    std::string track;             // its track's name ("Track 3" when it has none, as REAPER shows it)
    std::string track_key;         // 6-5-10b: the track's identity for its group's fold (its GUID,
                                   //   kept through a rename or a move); "" = its number
    int         track_num = 0;     // the track's 1-based number (the arrange's order)
    unsigned    track_rgb = 0;     // its colour 0xRRGGBB when has_color
    bool        has_color = false;
    double      pos = 0.0;         // project position (s)
    double      len = 0.0;         // item length (s)
    bool        muted = false;     // the item or its track is muted (greyed)
    int         copy_n = 0;        // NumberCopies: 1-based among its copies (0 = no copy)
    int         copy_of = 0;       //   how many copies (0 = no copy)
};

enum class Grouping : int { Track = 0, File = 1, None = 2 };
constexpr int kGroupingCount = 3;

// The grouping a stored value names; anything else = the default (Track).
inline Grouping GroupingFromInt(int v)
{
    return (v == 1) ? Grouping::File : (v == 2) ? Grouping::None : Grouping::Track;
}

// Copies: the items that play the same clip of the same file (path without case, as Windows
// names files; the clip as written) are numbered 1..n in time order (then track order). An item
// alone gets 0 / 0.
void NumberCopies(std::vector<ClipEntry>& entries);

// True when `query` (trimmed; empty = every row) is in the clip's, the file's or the track's name,
// ignoring ASCII case.
bool MatchesQuery(const ClipEntry& e, const std::string& query);

struct GroupView {
    std::string      label;              // "" = no header (Grouping::None)
    std::string      key;                // 6-5-10b: the group's fold key (GroupKey); "" = never folds
    bool             folded = false;     // 6-5-10b: only its header shows (ApplyFolds)
    unsigned         rgb = 0;            // the header's colour bar when has_color
    bool             has_color = false;
    std::vector<int> rows;               // indexes into the entries, in display order
};

struct ListView {
    std::vector<GroupView> groups;  // only groups with a row
    std::vector<int>       order;   // every shown row, top to bottom (the arrows walk it; a folded
                                    //   group's rows are not shown)
    int                    shown = 0;
    int                    total = 0;
};

// The list as shown: the rows the query keeps, grouped.
//   Track: one group per track in the arrange's order (its name, its colour), rows in time order;
//   File:  one group per file, by name (no case), rows in time order then track order;
//   None:  one group with no header, every row in time order then track order.
ListView BuildListView(const std::vector<ClipEntry>& entries, Grouping g, const std::string& query);

// ---- 6-5-10b: folded groups -------------------------------------------------------------------
// A group folds to its header (a click on it, or its triangle). The folds are view state: kept for
// the session, per project, never in the project and no undo point. A typed search opens every group
// it kept (they hold its matches) without changing the folds: cleared, the folded groups fold again.

// The keys of the folded groups ("t:" + a track's key, "f:" + a file's name without case).
using FoldSet = std::set<std::string>;

// A row's group key in this grouping: "t:" + track_key (or its number when it has none), "f:" + its
// file's name without case; "" in Grouping::None (no header, nothing to fold).
std::string GroupKey(const ClipEntry& e, Grouping g);

// Marks the folded groups of the list (none while `searching`) and takes their rows out of `order`.
void ApplyFolds(ListView& lv, const FoldSet& folded, bool searching);

// Expand all / Collapse all: whether either has something to do (both off while searching), and
// the folds after one (`fold` true = Collapse all). Only the groups of the list change; a group of
// another grouping keeps its fold.
bool CanExpandAll(const ListView& lv, const FoldSet& folded, bool searching);
bool CanCollapseAll(const ListView& lv, const FoldSet& folded, bool searching);
void SetAllFolds(const ListView& lv, bool fold, FoldSet* folded);
// One group's header clicked: folds it or opens it.
void ToggleFold(const std::string& key, FoldSet* folded);

// ---- 6-5-10b: REAPER's selection, focused in the list --------------------------------------------
// The row to focus after REAPER's item selection changed from `before` to `now` (rows of the list,
// in REAPER's order): the first row newly selected; -1 when no row was added (a deselection, or the
// same selection), or none is selected.
int SelectionFocus(const std::vector<int>& before, const std::vector<int>& now);

// The list flattened for drawing only what is on screen: one line per group header (groups with a
// label) and per row of an open group, top to bottom. `row` = -1 for a header.
struct ListLine {
    int group = 0;  // index into ListView::groups
    int row = -1;   // index into the entries, or -1 for the group's header
};
std::vector<ListLine> FlattenListView(const ListView& lv);

// Each line's top (content px from the list's top) for headers `header_h` and rows `row_h` high;
// `*total` = the whole list's height.
std::vector<float> LineTops(const std::vector<ListLine>& lines, float header_h, float row_h, float* total);

// The lines [*first, *last) that show between y0 and y1 (content px). Empty: *first == *last.
void VisibleLines(const std::vector<float>& tops, float total, float y0, float y1, int* first, int* last);

// The small line under the clip's name: the file (the track in File grouping), then the length.
std::string SubLine(const ClipEntry& e, Grouping g);

// "m:ss.s" (h:mm:ss.s from an hour), never negative: the row's position.
std::string FormatPosition(double seconds);
// "2.4 s": the row's length.
std::string FormatLength(double seconds);
// "copy 2/3" ("" for an item that is not a copy).
std::string CopyBadge(const ClipEntry& e);

// The arrows' dashed row: one step (delta -1 up, +1 down) from the dashed row `kb`, or from the
// current row `cur` when no row is dashed (-1), within `order`. With neither in the list, the first
// row. -1 for an empty list. Stops at both ends.
int StepRow(const std::vector<int>& order, int kb, int cur, int delta);

// 6-5-10b: the panel's height. It covers the 3D view, never the viewer's own controls: its top just
// under the menu button (`menu_bottom` + 8 px), its bottom just above the bottom-left controls
// (`ui_top`: the Model / Skeleton switch, or the status icon where there is no switch; the Tagging
// strip and the Video timeline are under them) less 8 px. In a window too short for a usable list
// (under 160 px between them), it reaches down over the bottom-left controls, then up over the menu
// button, but never under `view_bottom` (the strip's top: the strip is never covered) nor above 0.
struct PanelSpan {
    float top = 0.0f;
    float bottom = 0.0f;
};
PanelSpan PanelVerticalSpan(float menu_bottom, float ui_top, float view_bottom, float dp);

// The panel's width in a view `view_w` px wide (`dp` = the UI scale): 270 px, 70 % of the view
// when that is less, but never under 200 px while the view has room for it; never more than the
// view less the room of the close button beside its right edge (28 px: the button, its gap and a
// margin). Continuous: no jump while the window is resized.
float PanelWidth(float view_w, float dp);

// 6-5-10b review: the open list's round button sits wholly OUTSIDE the panel, 2 px right of its
// right edge (`panel_w`), so it never covers the rows' right end nor the scrollbar (a click there
// closed the list). Its centre's x; `btn` = its diameter.
float OpenButtonCentreX(float panel_w, float btn, float dp);

// 6-5-10b review: while the list has the keyboard (a row was the last thing clicked), a character
// key (a letter, a digit, the punctuation keys; with or without Shift; never with Ctrl / Alt) is
// typed into the search, not run as a RAV or REAPER shortcut: typing "walk" searches, L does not
// close the list. Space (play), the numpad and the function / edit keys stay shortcuts. `vk` = the
// Windows virtual-key code.
bool ListTypesKey(unsigned vk, bool ctrl, bool alt);

// The button's centre height: half-way between the menu button's bottom (`menu_bottom`) and the
// Model / Skeleton switch's top (`switch_top`; the status icon's in Video / Library view); when they are under 140 px apart, just under the
// menu button (6 px gap). `btn` = its diameter.
float ButtonCentreY(float menu_bottom, float switch_top, float btn, float dp);

// True when the button fits between the menu button and the switch (6 px gap above, 4 px below);
// otherwise it is not drawn (never over the switch) and the shortcut / Esc still open and close.
bool ButtonFits(float menu_bottom, float switch_top, float btn, float dp);

// ---- 6-5-10c: the menu button (the burger) above the list -------------------------------------
// Antho 2026-10-14: the burger's frame was far too big for its three lines, and the list's top sat
// against it. The burger is the corner icons' 24 px button (an 18 px icon, 3 px around it), framed
// tight over the 3D view; its middle is the top row's (`row_mid`: the FPS readout's text line, where
// the keyboard and full screen icons are centred), its left edge as far from the window's as its top. Open, the
// menu's panel (8 px padding) holds it at the very same place. `bottom` is the list's `menu_bottom`
// (PanelVerticalSpan keeps 8 px under it), `trail` the first x the view switch may use (8 px right of it).
struct MenuButtonBox {
    float x = 0.0f, y = 0.0f;              // its top-left
    float size = 0.0f;                     // its side
    float bottom = 0.0f;                   // y + size
    float panel_x = 0.0f, panel_y = 0.0f;  // the open menu's panel (the burger at its 8 px padding)
    float trail = 0.0f;                    // its right edge + 8 px
};
MenuButtonBox MenuButtonAt(float row_mid, float dp);

// ---- 6-5-10c: a project load REAPER reports, and the list ---------------------------------------
// REAPER's projectconfig hook says a project state is being loaded, but not only when the user opens
// a project in the tab shown: REAPER also loads states for other purposes, some it does not name. The
// list (open or closed) starts afresh, closed with an empty search (Antho, 2026-10-14), only when
// another project shows in the tab once the load is over: decided at the next frame from what the tab
// showed before the load and what it shows now. Otherwise nothing changes (and the grouping and folds
// the load dropped come back).
struct ProjectLook {
    const void* proj = nullptr;  // the tab shown (its ReaProject*)
    std::string path;            // its file ("" = never saved)
    int         items = 0;       // its media items, all of them
};
enum class LoadVerdict {
    Stays,      // the same project and file, its items still there: nothing was opened
    OtherTab,   // another project tab shows now (a project opened in a new tab)
    OtherFile,  // another file in this tab (File > Open; File > New after a saved project)
    Emptied,    // the tab's project lost every item (File > New)
    Replaced,   // the same file opened again: most of the items the list showed are gone
};
// `listed`: how many items the list showed when the load began (0 when it was closed), `listed_still`:
// how many of them are still items of the project shown now.
LoadVerdict ProjectLoadVerdict(const ProjectLook& before, const ProjectLook& now, int listed, int listed_still);
// The verdict in the log's words ("another file", ...).
const char* LoadVerdictText(LoadVerdict v);

// 6-5-10c: the note a close of the list writes in the log after its cause: the view, and when the
// close came within 10 s of a row click (which jumps and loads an item), how long after it:
// "Main view, 3 frames (0.21 s) after a row click". `frames_since_click` < 0: no click yet.
std::string CloseNote(const std::string& view, long long frames_since_click, double seconds_since_click);

}  // namespace cliplist
}  // namespace rav
