// SPDX-License-Identifier: MIT
//
// Spread selection to marker type (13-68 v2, REAPER): the REAPER actions "RAV: Tagging - Attach selected items to
// selected marker (this animation / all instances)" and "RAV: Tagging - Add selected items to stack". It is
// Populate, told which sounds to use: the selection becomes the marker type's library (category Attached, named
// after the type; sound_spread_plan.h builds its nodes), assigned to the rules writing that type (the selected
// marker's animation item and its linked copies, or every RAV item with that type), then Populate places it on
// those rules only (PopulateItemsCounted only_rids). The first pile moves onto the selected marker and stays
// attached to it; the other originals move after the project end (the source zone). The type = the RAV project
// marker selected in REAPER (its label clicked). One undo point per run; the report shows as a toast on
// REAPER's main window (reaper_toast.h).

#pragma once

#include <string>

namespace rav {

enum class SpreadScope { ThisAnimation, AllInstances };

#ifdef _WIN32
// Resolves this module's extra REAPER functions (plugin load).
void InitSoundSpreadSelection(void* (*get_func)(const char* name));
#endif

// Runs the action: one undo point "RAV: Spread selection to <type> (this animation)" / "(all instances)":
// the first pile onto the selected marker (attached), the others to the source zone, the type's Attached library
// made or its content replaced, the assignment, Populate. Its report (or why it cannot run) as a toast.
// Returns that text.
std::string SpreadSelectionToMarkerType(SpreadScope scope);
// Add to stack (Antho 2026-10-18): no marker to select. The selection holds one item of a stack (an Attach's
// original or a placed copy) and the new items, put where they go against it. They become a new layer of that
// stack's Attached library (its root becomes or stays a Blend of the existing content and a Random of the new
// piles) at their offset from the stack's event (top item's snap minus the event); the pile nearest the event
// stays there, attached; the other piles go to the source zone; then Populate (that library's rules only),
// wherever the library is assigned. One undo point "RAV: Add selection to stack <type>".
std::string AddSelectionToStack();

}  // namespace rav
