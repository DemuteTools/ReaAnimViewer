// SPDX-License-Identifier: MIT
//
// Spread selection to marker type (13-68 v2 / v3, REAPER): the REAPER actions "RAV: Tagging - Attach selected items
// to selected marker" and "RAV: Tagging - Add selected items to stack", each "(this animation)" / "(all animations
// with this marker)". It is Populate, told which sounds to use: the selection becomes the marker kind's library
// (category Attached, one per kind for both sides, side-free name "FS"; sound_spread_plan.h builds its nodes),
// assigned to the rules writing that kind on either side (the selected marker's animation item and its linked
// copies, or every RAV item with that kind), then Populate places it on those rules only (PopulateItemsCounted
// only_rids). The first pile moves onto the selected marker and stays attached to it; the other originals move after
// the project end (the source zone). The kind = the RAV project marker selected in REAPER (its label clicked), read
// from its rule. One undo point per run; the report shows as a toast on REAPER's main window (reaper_toast.h).

#pragma once

#include <string>

namespace rav {

enum class SpreadScope { ThisAnimation, AllInstances };

#ifdef _WIN32
// Resolves this module's extra REAPER functions (plugin load).
void InitSoundSpreadSelection(void* (*get_func)(const char* name));
#endif

// Runs the action: one undo point "RAV: Spread selection to <kind> (this animation)" / "(all animations with this
// marker)": the first pile onto the selected marker (attached), the others to the source zone, the kind's Attached
// library made or its content replaced, the assignment (both sides), the earlier run's sounds on the library's
// events to the source zone, Populate. Its report (or why it cannot run) as a toast. Returns that text.
std::string SpreadSelectionToMarkerType(SpreadScope scope);
// Add to stack (Antho 2026-10-18, 13-68 v3): no marker to select. The selection holds the new items and one or more
// items of one marker's stack (an Attach's original or a placed copy; several markers: refused), the new ones put
// where they go against it. They become a new layer of that stack's Attached library at their offset from the
// stack's event (top item's snap minus the event): all animations = in the shared content (its root becomes or stays
// a Blend); this animation = under the library's RAV-owned per-animation Switch, the stack's animation item and its
// linked copies given their value. The pile nearest the event stays there, attached; the other piles go to the
// source zone; then Populate (that library's rules only). One undo point "RAV: Add selection to stack <kind> (...)".
std::string AddSelectionToStack(SpreadScope scope);

}  // namespace rav
