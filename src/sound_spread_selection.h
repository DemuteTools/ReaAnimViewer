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
#include <vector>

#include "attach_manager_plan.h"

class MediaItem;  // REAPER SDK

namespace rav {

enum class SpreadScope { ThisAnimation, AllInstances };
// 13-68e: both sides of the marker kind (the default), or the chosen marker's side only (the RAV - Side Switch).
enum class SpreadSide { Both, MarkerSide };
// 13-68e: the marker the Attach manager chose (instead of the clicked label): its animation item, its name as its rule
// writes it, its project position.
struct SpreadMarker {
    MediaItem*  item = nullptr;
    std::string name;
    double      pos = 0.0;
};

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

// ---- 13-68e: the Attach manager's gestures (attach_manager_ui.h) ------------------------------------------------

// Attach with the window's marker (null = the clicked label, as the actions) and side: "<side> only" puts the selection
// in the side's value of the library's RAV - Side Switch, assigns it to that side's rules only and Populates them;
// the other side keeps what it had by default (Keeps its sound), or plays nothing with `other_none`; every rule
// holding the library gets its side's value. One undo point.
// 13-68e v4: `other_none` (with MarkerSide) = the other side plays nothing: its value of the RAV - Side Switch emptied,
// its placed sounds of the library removed as a re-run removes them (originals to the source zone).
std::string SpreadSelectionToMarker(SpreadScope scope, SpreadSide side, const SpreadMarker* marker, bool other_none = false);
// Add to stack on one side: the layer under the RAV - Side Switch (sound_spread_plan.h SideLayerTop), Populate on the
// stack's side only.
std::string AddSelectionToStackSide(SpreadScope scope, SpreadSide side, bool other_none = false);

// The window's snapshot of the project (attach_manager_plan.h) and the animation items it names (same order as
// snap.anims). Never writes.
struct AttachManagerState {
    am::Snapshot            snap;
    std::vector<MediaItem*> anims;
};
void ReadAttachManager(AttachManagerState* out);
// The cheap part, every frame: the project's state count, the time selection, the selected items. Equal = nothing to
// read again (the clicked label is polled apart: ClickedMarkersKey).
std::string AttachManagerSignature();
// The RAV project markers whose label is selected in REAPER, as a key ("" = none): their change reads the project again.
std::string ClickedMarkersKey();

// An event of the Detach list: its animation item's GUID, rule id and clip time.
struct DetachTarget {
    std::string owner;
    std::string rid;
    double      t = 0.0;
    std::string label;  // "FS L 1.70 s" (the report's)
};
// Detach from these markers: their sounds leave them (Keep: the items stay in place, untagged, no longer following;
// Delete: deleted, but a library's source item goes to the source zone), the markers then stay empty (Populate leaves
// them). One undo point "RAV: Detach from <marker>" / "... from N markers"; the report as a toast.
std::string DetachFromMarkers(const std::vector<DetachTarget>& targets, bool keep);
// Remove from library: the variation `node` leaves the Attached library `lib_uid` with every sound placed from it
// (Keep: in place, untagged, those markers get nothing else; Delete: deleted, then Populate draws those markers again
// from the variations left; an emptied library leaves them empty). One undo point "RAV: Remove <name> from the <lib>
// library"; the report as a toast.
// 13-68e v4: `side` ('L' / 'R') = from that side only (a variation both sides share is split through RAV - Side, the
// other side gets its own copy); Delete redraws those markers from what is left on their side, else they stay empty;
// the undo point then reads "RAV: Remove <name> from the <lib> library (L only)".
std::string RemoveFromAttachedLibrary(const std::string& lib_uid, const std::string& node, bool keep, char side = 0);
// Selects these items in REAPER (every other item unselected); the arrange scrolls nowhere. Returns how many.
int SelectItemsByGuid(const std::vector<std::string>& guids);

}  // namespace rav
