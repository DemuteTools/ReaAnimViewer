// SPDX-License-Identifier: MIT
//
// Story 6-5-10 -- the animation clip list, the REAPER and Dear ImGui side (the validated mockup
// _bmad-output/planning-artifacts/ux-designs/maquette-liste-clips-animation.html). A small round
// button on the left of every view (RAV, Tagging, Video, Library), half-way between the menu and
// the Model / Skeleton switch, opens a panel over the left of the view (never reframing it, never
// over the menu button, the bottom-left controls, the strip or the right-hand panels): every
// RAV item of the project, grouped by track (default), by file or not at all, with a search. A
// click on a row (or Enter on the arrows' dashed row) puts the edit cursor at the item's start and
// selects that item alone in REAPER, so it becomes RAV's current item in every view; the panel
// stays open until the user closes it. The row RAV shows (the current item, story 11-7's rule) is
// lit and follows REAPER; an item selected in REAPER gets the dashed row and comes into view
// (6-5-10b). Track and File groups fold (session view state; a search opens those with matches).
//
// Muted items and items on muted tracks (or on a lane that does not play) are listed greyed. The
// list is read again only when REAPER's project state count changes (or the project tab), and
// only while the panel is open. The grouping is kept in the project (a line of the .rpp written
// when the project is saved: no undo point, no unsaved-change mark); the panel starts closed with
// an empty search when another project is opened in the tab (6-5-10c: decided once REAPER's load is
// over, cliplist::ProjectLoadVerdict). The pure half is src/clip_list_plan.h.

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
// Opens (the search takes the keyboard) or closes the panel: the only way the list opens or closes.
// `reason`: what asked (the button, the shortcut, Esc...). 6-5-10c: every close is kept in the
// "Copy error log" ring (a warning line, so a release build keeps it too) with the view and how
// soon after a row click it came: a list that vanishes says why.
void SetClipListOpen(bool open, const char* reason);

// The item RAV shows this frame (nullptr = none): the lit row. Called by the render tick.
void ClipListSetCurrentItem(MediaItem* item);

// The button and, open, the panel. `view_w`: the view's width from the window's left edge (left
// of the Tagging / Video panel); `menu_bottom`: the menu button's bottom; `ui_top`: the top of the
// bottom-left controls (the Model / Skeleton switch, or the status icon where there is no switch);
// `view_bottom`: the view's bottom (the Tagging strip's / Video timeline's top, else the window's).
// 6-5-10b: the panel covers the 3D view only, between `menu_bottom` and `ui_top`
// (cliplist::PanelVerticalSpan), and the button goes half-way between them. `key`: the shortcut's
// label for the tooltip. `view`: the view shown, named in the log ("Main", "Tagging"...). True when
// the panel opened this frame (the menu closes).
bool DrawClipList(float view_w, float menu_bottom, float ui_top, float view_bottom, const char* key, const char* view);

// The panel's and the button's windows this frame (nullptr when not drawn): kept above the other
// viewer windows after EndFrame.
ImGuiWindow* ClipListPanelWindow();
ImGuiWindow* ClipListButtonWindow();
// 6-5-10c: once the windows are in their final order (after the above), the open panel is checked:
// shown, and under no other window but its button, `menu_open` (the options menu when open, else
// nullptr), popups and tooltips. A finding is written to the log once while it lasts.
void ClipListAfterFrame(ImGuiWindow* menu_open);

// True when the panel has the keyboard (its search field or a row was the last thing clicked) and
// this key, without Ctrl / Alt, is one of its own: Up, Down, Enter, Esc, and (6-5-10b review) a
// character key (cliplist::ListTypesKey), typed into the search. The viewer takes it then.
bool ClipListTakesKey(unsigned vk, bool ctrl, bool shift, bool alt);
// True when the panel has the keyboard (see above).
bool ClipListHasFocus();

}  // namespace rav

#endif  // _WIN32
