// SPDX-License-Identifier: MIT
//
// 13-35 (ImGui): Sync placed sounds in the Library view (mockup maquette-sync-placed-sounds-13-35.html): the Placed
// sounds card at the top of the tree, the ↻ mark of a node whose placed sounds are behind, the node panel's pending
// line, and the two dialogs (where to apply it: the animation items; then the sounds edited by hand: Keep / Overwrite).
// The gesture is Populate's (sound_populate.h SyncPlacedSounds), one undo point.

#pragma once

#ifdef _WIN32

#include <string>

namespace rav {

// Plugs the card and the marks into the library tree (LibraryTreeHooks::sync_card / sync_mark); called each frame.
void MergeSyncTreeHooks();
// The card of library `lib` (nothing when no placed sound plays from it).
void DrawPlacedSoundsCard(const std::string& lib);
// The ↻ N mark of `node`, its right edge at `right_x`, centred on `cy`; its click opens the dialog limited to the node.
// Returns the width it took (0 = none).
float DrawSyncMark(const std::string& lib, const std::string& node, float right_x, float cy);
// The node panel's line "Not yet on N placed sounds: <change>  [Sync this node…]" (nothing when none is behind).
void DrawSyncPendingLine(const std::string& lib, const std::string& node);
// Opens the where-dialog for library `lib` (`node` "" = every change of the library).
void OpenSyncDialog(const std::string& lib, const std::string& node);
// The dialogs (modal cards), drawn each frame of the Library view; the gesture runs after them.
void DrawSyncDialogs();

}  // namespace rav

#endif  // _WIN32
