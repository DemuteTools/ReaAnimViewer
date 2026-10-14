// SPDX-License-Identifier: MIT
//
// Story 6-5-10 -- the animation clip list (the round button on the left of every view), its pure
// half: the rows' copies, the search, the three groupings and their order, the keyboard's dashed
// row, the texts of a row, and where the panel and its button go at any window size. No REAPER, no
// Windows, no Dear ImGui. The REAPER reading and the drawing are src/clip_list_ui.cpp; the host test
// is tests/clip_list_plan_test.cpp.

#pragma once

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
    unsigned         rgb = 0;            // the header's colour bar when has_color
    bool             has_color = false;
    std::vector<int> rows;               // indexes into the entries, in display order
};

struct ListView {
    std::vector<GroupView> groups;  // only groups with a row
    std::vector<int>       order;   // every shown row, top to bottom (the arrows walk it)
    int                    shown = 0;
    int                    total = 0;
};

// The list as shown: the rows the query keeps, grouped.
//   Track: one group per track in the arrange's order (its name, its colour), rows in time order;
//   File:  one group per file, by name (no case), rows in time order then track order;
//   None:  one group with no header, every row in time order then track order.
ListView BuildListView(const std::vector<ClipEntry>& entries, Grouping g, const std::string& query);

// The list flattened for drawing only what is on screen: one line per group header (groups with a
// label) and per row, top to bottom. `row` = -1 for a header.
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

// The panel's width in a view `view_w` px wide (`dp` = the UI scale): 270 px, 70 % of the view
// when that is less, but never under 200 px while the view has room for it; never more than the
// view less the room of the close button that sticks out of its right edge (16 px). Continuous:
// no jump while the window is resized.
float PanelWidth(float view_w, float dp);

// The button's centre height: half-way between the menu button's bottom (`menu_bottom`) and the
// Model / Skeleton switch's top (`switch_top`); when they are under 140 px apart, just under the
// menu button (6 px gap). `btn` = its diameter.
float ButtonCentreY(float menu_bottom, float switch_top, float btn, float dp);

// True when the button fits between the menu button and the switch (6 px gap above, 4 px below);
// otherwise it is not drawn (never over the switch) and the shortcut / Esc still open and close.
bool ButtonFits(float menu_bottom, float switch_top, float btn, float dp);

}  // namespace cliplist
}  // namespace rav
