// SPDX-License-Identifier: MIT
//
// Spread selection to marker type (13-68 v2, REAPER): the REAPER actions "RAV: Spread selected items to
// selected marker type (this animation / all instances)" and "RAV: Add selected items to stack on selected
// marker type (...)". It is Populate, told which sounds to use: the selection becomes the marker type's
// library (category Attached, named after the type; sound_spread_plan.h builds its nodes), assigned to
// the rules writing that type (the selected marker's animation item and its linked copies, or every RAV
// item with that type), then Populate places it (PopulateItemsCounted). The selected originals move
// after the project end (the source zone). The type = the RAV project marker selected in REAPER (its
// label clicked). One undo point per run; the report shows as a toast on REAPER's main window
// (reaper_toast.h).

#pragma once

#include <string>

namespace rav {

enum class SpreadScope { ThisAnimation, AllInstances };

#ifdef _WIN32
// Resolves this module's extra REAPER functions (plugin load).
void InitSoundSpreadSelection(void* (*get_func)(const char* name));
#endif

// Runs the action: one undo point "RAV: Spread selection to <type> (this animation)" / "(all instances)":
// the originals moved to the source zone, the type's Attached library made or its content replaced, the
// assignment, Populate. Its report (or why it cannot run) as a toast. Returns that text.
std::string SpreadSelectionToMarkerType(SpreadScope scope);
// Add to stack: the selection is a new layer of the type's Attached library: its root becomes (or stays) a
// Blend of the existing content and a Random of the selection's piles, offset by the selection's top item's
// snap minus its nearest marker of the type (on the selected marker's item); then Populate. No Attached
// library for the type yet: as SpreadSelectionToMarkerType. One undo point "RAV: Add selection to stack on
// <type> (this animation)" / "(all instances)".
std::string AddSelectionToStack(SpreadScope scope);

}  // namespace rav
