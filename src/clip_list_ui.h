// SPDX-License-Identifier: MIT
//
// Story 6-5-10 -- the animation clip list, the REAPER and Dear ImGui side (the validated mockup
// _bmad-output/planning-artifacts/ux-designs/maquette-liste-clips-animation.html). A small round
// button on the left of every view (RAV, Tagging, Video, Library), half-way between the menu and
// the Model / Skeleton switch, opens a panel over the left of the view (never reframing it): every
// RAV item of the project, grouped by track (default), by file or not at all, with a search. A
// click on a row (or Enter on the arrows' dashed row) puts the edit cursor at the item's start and
// selects that item alone in REAPER, so it becomes RAV's current item in every view; the panel
// stays open. The row RAV shows (the current item, story 11-7's rule) is lit and follows REAPER.
//
// Muted items and items on muted tracks (or on a lane that does not play) are listed greyed. The
// list is read again only when REAPER's project state count changes (or the project tab), and
// only while the panel is open. The grouping is kept in the project (a line of the .rpp written
// when the project is saved: no undo point, no unsaved-change mark); the panel opens closed with an
// empty search after a project load. The pure half is src/clip_list_plan.h.

#pragma once

#ifdef _WIN32

#include "reaper_api.h"

struct ImGuiWindow;

namespace rav {

// The projectconfig line that keeps the grouping, and the optional REAPER functions (item
// selection, the undo preferences). Once, at load. QuitClipList unregisters at unload.
void InitClipList(reaper_plugin_info_t* rec);
void QuitClipList();

bool ClipListOpen();
// Opens (the search takes the keyboard) or closes the panel.
void SetClipListOpen(bool open);

// The item RAV shows this frame (nullptr = none): the lit row. Called by the render tick.
void ClipListSetCurrentItem(MediaItem* item);

// The button and, open, the panel. `view_w`: the view's width from the window's left edge (left
// of the Tagging / Video panel), `view_h`: the window's height; `menu_bottom` / `switch_top`: the
// menu button's bottom and the Model / Skeleton switch's top (the button goes half-way); `key`: the
// shortcut's label for the tooltip. True when the panel opened this frame (the menu closes).
bool DrawClipList(float view_w, float view_h, float menu_bottom, float switch_top, const char* key);

// The panel's and the button's windows this frame (nullptr when not drawn): kept above the other
// viewer windows after EndFrame.
ImGuiWindow* ClipListPanelWindow();
ImGuiWindow* ClipListButtonWindow();

// True when the panel has the keyboard (its search field or a row was the last thing clicked) and
// this key, without Ctrl / Alt, is one of its own: Up, Down, Enter, Esc. The viewer takes it then.
bool ClipListTakesKey(unsigned vk, bool ctrl, bool shift, bool alt);
// True when the panel has the keyboard (see above).
bool ClipListHasFocus();

}  // namespace rav

#endif  // _WIN32
