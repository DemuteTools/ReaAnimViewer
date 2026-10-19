// SPDX-License-Identifier: MIT
//
// Attach and Add to pile (13-68 v2 / v3, 13-68e v5, REAPER): the REAPER actions "RAV: Tagging - Attach selected items
// to selected marker" and "RAV: Tagging - Add selected items to stack", each "(this animation)" / "(all animations
// with this marker)" (= Apply to Anim / Project), and the Attach manager's gestures (attach_manager_ui.h). It is
// Populate, told which sounds to use: the selection becomes the marker kind's library (category Attached, one per kind
// for both sides, side-free name "FS"; sound_spread_plan.h builds its nodes, attach_manager_plan.h places them by
// scope: the animation's own case of the RAV - <kind> animations Switch, L and R through RAV - Side once separated),
// assigned to the rules writing that kind on either side, then Populate places it on those rules only
// (PopulateItemsCounted only_rids). The first pile moves onto the marker and stays attached to it; the other originals
// move after the project end (the source zone). One undo point per gesture; the report shows as a toast on REAPER's
// main window (reaper_toast.h).
//
// The watcher (AttachWatchTick, a REAPER timer: also with the window closed) makes a gesture made in REAPER on one
// instance reach its linked instances, as the project's Apply to setting says: only a delete (Antho 2026-10-20), which
// removes the layer from them and from the library in scope; nothing else follows (selection, volume, fades, moves).
// The propagation is one RAV undo point right after the user's.

#pragma once

#include <string>
#include <vector>

#include "attach_manager_plan.h"

class MediaItem;  // REAPER SDK

namespace rav {

// The four 13-68 actions (bindings kept): this animation = Apply to Anim, all animations with this marker = Project.
enum class SpreadScope { ThisAnimation, AllInstances };
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

// The actions: Attach with the clicked RAV marker label; Add to pile ("Add to stack") from the selected instance.
std::string SpreadSelectionToMarkerType(SpreadScope scope);
std::string AddSelectionToStack(SpreadScope scope);

// ---- 13-68e v5: the Attach manager's gestures (attach_manager_ui.h) ---------------------------------------------

// Attach: the selection onto `marker` (null = the clicked label) and, in Anim / Project, onto every marker of its kind
// in scope (L and R together until separated, else the marker's side), replacing what they had (the previous original
// to the source zone). Item: that marker only, the library unchanged. One undo point "RAV: Attach to FS L (...)".
std::string AttachToMarker(am::Scope scope, const SpreadMarker* marker);
// Add to pile: the selected new items become a layer of every linked instance in scope, at their offset from the
// selected instance's marker. Item: that instance only. One undo point "RAV: Add to pile FS (...)".
std::string AddToPile(am::Scope scope);
// Separate L and R (Anim / Project, from an L or R marker): the same sound, two groups from now on.
std::string SeparateLR(am::Scope scope, const SpreadMarker& marker);
// Merge L and R: both sides play the marker's side's sound; the other side's sounds are replaced.
std::string MergeLR(am::Scope scope, const SpreadMarker& marker);
// Reshuffle (Antho 2026-10-20): the selected copies RAV placed drawn again, only their events (`events`: animation item
// GUID, rule id, clip time; am::ReshuffleTargets), as RAV's Reshuffle draws (no other sound touched). One undo point
// "RAV: Reshuffle N sounds" (`sounds` = N); the report as a toast.
struct ReshuffleTarget {
    std::string owner;
    std::string rid;
    double      t = 0.0;
};
std::string ReshuffleSelected(const std::vector<ReshuffleTarget>& events, int sounds);
// The project's Apply to setting (one undo point "RAV: Attach manager - Apply to Anim"). False when unchanged.
bool SetApplyTo(am::Scope scope);

// The window's snapshot of the project (attach_manager_plan.h) and the animation items it names (same order as
// snap.anims). Never writes.
struct AttachManagerState {
    am::Snapshot            snap;
    std::vector<MediaItem*> anims;
};
void ReadAttachManager(AttachManagerState* out);
// The cheap part, every frame: the project's state count, the time selection, the selected items, the store. Equal =
// nothing to read again (the clicked label is polled apart: ClickedMarkersKey).
std::string AttachManagerSignature();
// The RAV project markers whose label is selected in REAPER, as a key ("" = none): their change reads the project again.
std::string ClickedMarkersKey();

// The watcher's tick (a REAPER timer, main thread): the linked selection and the propagated gestures. Never throws.
void AttachWatchTick();
// REAPER's post-command hook: a delete action ran (the next delete is new even under the same undo name).
void AttachWatchPostCommand(int command, int flag);

}  // namespace rav
