// SPDX-License-Identifier: MIT
//
// Epic 13 (13-15b) -- the Sound assignment window of the Tagging view: a movable, resizable floating window
// (mockup ux-designs/epic-13-sound-library/maquette-assignation-sons.html) where the library and the Switch
// values of the item's bone rules, auto detection types and the item itself are read through the layers
// Kit > Project > Character > Item > Event (sound_assign.h) and edited, several targets at once, one undo
// point per gesture. 13-15b has the Item scope; Project and Character (13-15c) show a placeholder line.
// What it shows is assign_view_model.h's (pure, host-tested). The Tagging strip keeps one summary line and
// an "Assign..." button per rule / auto type (DrawAssignSummaryRow).

#pragma once

#ifdef _WIN32

#include <windows.h>

#include <string>
#include <vector>

#include "bone_events.h"  // Block

class MediaItem;  // REAPER SDK

namespace rav {

struct TaggingModel;

// Opens the window at the Item scope on `item` (the Tagging view's shown item), the item's rules `blocks`
// (indices into its blocks) selected and their section unfolded; no blocks = the window as it was.
void OpenAssignWindow(MediaItem* item, const std::vector<int>& blocks);
// 13-43: opens the window from the Library on its Overview at the Project scope, the cells that play
// `library_uid` lit (until the window closes or opens another way).
void OpenAssignOverview(const std::string& library_uid);
// 13-43: where a library plays now (assign_view_model.h's PlayedByText over the project's characters);
// "" when nowhere.
std::string AssignPlayedByText(const std::string& library_uid);
void CloseAssignWindow();
// Open (Antho 2026-10-12: a window of its own, anywhere on the screen, shown in every view while the
// viewer is; hidden, not closed, while the viewer is hidden).
bool AssignWindowIsOpen();
// The window is open and has the keyboard focus.
bool TaggingAssignWindowOpen();
// Review 13-15b: the keys the window takes while it has the focus (Esc, the arrows, Enter, and Delete /
// Backspace so they never reach the Tagging view's marker delete); every other key (Space, Ctrl+Z...)
// stays REAPER's. Typing in its filter is a text field (WantTextInput) as anywhere.
bool TaggingAssignTakesKey(unsigned vk, bool ctrl, bool shift, bool alt);
// Once per frame while the Tagging view is shown: the window follows its item.
void DrawAssignWindow(const TaggingModel& m);
// The viewer, once per render tick after its own frame (its GL context current): the window's frame in its
// own OS window and ImGui context (float_ui_window.h), then its queued gestures. Restores both contexts.
void DrawAssignFloat(HWND viewer, HDC viewer_dc, HGLRC rc);
// The viewer is hidden (no tick runs): the window hides with it.
void HideAssignFloat();
// The viewer closes (its GL and ImGui contexts current): the window and its context are freed.
void DestroyAssignFloat();
// The key routing (the viewer's accelerator hook): a message for the window, and whether it types text.
bool AssignFloatOwns(HWND hwnd);
// 13-59 review: the window's OS window (nullptr when it has none): the busy card's end drops the
// presses made on it too.
HWND AssignFloatHwnd();
bool AssignFloatWantsText();

// The Tagging strip's line for some blocks of the item: "<label> <dot> FS Hero > Run > Boots" (inherited
// parts grey) and an "Assign..." button that opens the window on `open_blocks` (empty = the button is off,
// with the tip "Detect first, or set it at Project / Character level"). `blocks` are the blocks to summarize
// (the item's own, or the ones a new marker of that type would be).
void DrawAssignSummaryRow(const char* id, MediaItem* item, const std::vector<Block>& blocks,
                          const std::vector<int>& open_blocks, const char* label);

// The block indices that go with block `b` of `blocks`: both sides of its marker pair, else itself.
std::vector<int> RuleGroupBlocks(const std::vector<Block>& blocks, int b);

}  // namespace rav

#endif  // _WIN32
