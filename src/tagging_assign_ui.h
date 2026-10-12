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

#include <vector>

#include "bone_events.h"  // Block

class MediaItem;  // REAPER SDK

namespace rav {

struct TaggingModel;

// Opens the window at the Item scope on `item` (the Tagging view's shown item), the item's rules `blocks`
// (indices into its blocks) selected and their section unfolded; no blocks = the window as it was.
void OpenAssignWindow(MediaItem* item, const std::vector<int>& blocks);
void CloseAssignWindow();
// Open, whichever view shows (it hides, not closes, while another view is shown).
bool AssignWindowIsOpen();
// The window is drawn and has the keyboard focus.
bool TaggingAssignWindowOpen();
// Review 13-15b: the keys the window takes while it has the focus (Esc, the arrows, Enter, and Delete /
// Backspace so they never reach the Tagging view's marker delete); every other key (Space, Ctrl+Z...)
// stays REAPER's. Typing in its filter is a text field (WantTextInput) as anywhere.
bool TaggingAssignTakesKey(unsigned vk, bool ctrl, bool shift, bool alt);
// Once per frame while the Tagging view is shown, after its panel and before the picked gestures run.
void DrawAssignWindow(const TaggingModel& m);

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
