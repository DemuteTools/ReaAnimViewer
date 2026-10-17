// SPDX-License-Identifier: MIT
//
// Spread selection to marker type (13-68, REAPER): the two REAPER actions "RAV: Spread selected items
// to selected marker type (this animation / all instances)". The selected sound items are read as
// blocks (items overlapping in time = one pile = one variation with its layers); the RAV project
// marker selected in REAPER (its label clicked) gives the type (its name, sound_attach_plan.h
// SameMarkerType). The piles are moved onto the first markers of that type of the selected marker's
// animation item and attached; every other marker of the type (that item's, or every RAV item's)
// gets a spread copy of one pile, its top item's snap on the marker. No Library node. One undo
// point per run; the report shows as a toast on REAPER's main window (reaper_toast.h).
// The pure logic: sound_attach_plan.h (PlanSpreadSelection, ChooseLane, SpreadSelectionReport).

#pragma once

#include <string>

namespace rav {

enum class SpreadScope { ThisAnimation, AllInstances };

#ifdef _WIN32
// Resolves this module's extra REAPER functions (plugin load).
void InitSoundSpreadSelection(void* (*get_func)(const char* name));
#endif

// Runs the action: one undo point "RAV: Spread selection to <marker> (this animation)" / "(all
// instances)" when it writes; its report (or why it cannot run) as a toast. Returns that text.
std::string SpreadSelectionToMarkerType(SpreadScope scope);

}  // namespace rav
