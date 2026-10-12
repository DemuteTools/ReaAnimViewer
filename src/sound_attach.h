// SPDX-License-Identifier: MIT
//
// Attach (13-10, REAPER): attaches the selected sound items to the shown animation item's events.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-10
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// Each gesture is ONE undo point (SoundUndoScope), writes the RAV_SND tag of every item it
// attaches or places and the animation item's RAV_SOUNDS, and never replaces nor removes an
// attached sound. The pure logic: sound_attach_plan.h.

#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

#include "event_list.h"
#include "reaper_api.h"
#include "sound_attach_plan.h"
#include "sound_model.h"

namespace rav {

// Resolves this module's extra REAPER functions (13-10 D2; plugin_main.cpp).
void InitSoundAttach(void* (*get_func)(const char* name));

// Attaches the selected (non-RAV) items to the nearest events of `anim_item`: one undo point.
SoundReport AttachSelectedTo(MediaItem* anim_item);

// The events a pile may attach to: the shown events of kind Detected or User, at every project
// time of their clip time.
std::vector<AttachEvent> AttachEventsOf(const std::vector<ShownEvent>& shown, const ItemClipMap& map);

// What the Attach menu needs of REAPER's item selection (read when the menu opens).
struct AttachSelectionInfo {
    int         eligible = 0;
    int         skipped = 0;
    std::string skip_note;      // "2 skipped: placed by RAV / folder item" ("" = none)
    int         attached = 0;   // eligible items that are attached (Promote)
};
AttachSelectionInfo ReadAttachSelection();

// A gesture's outcome: a one-line note and its report.
struct AttachResult {
    bool        wrote = false;
    std::string note;
    SoundReport report;
    std::string undo_desc;  // the gesture's undo point description, when it wrote
};
// Attach to nearest event: the eligible selected items as one pile, on the event nearest to its
// anchor (any rule).
AttachResult AttachToNearest(MediaItem* anim_item, const std::vector<AttachEvent>& events);
// Attach Selected to Markers: each group of overlapping items on its nearest event of rule `block`.
AttachResult AttachToMarkers(MediaItem* anim_item, const std::vector<AttachEvent>& events, int block);
// Spread to: the pile copied (tagged spread) onto every other event of rule `block` (D5, D6).
AttachResult SpreadTo(MediaItem* anim_item, const std::vector<AttachEvent>& events, int block);
// Promote to Library: the sounds attached to the topmost selected attached item's event copied
// after the project end and added to the library (`value_uid` = the root Switch's value) (D7, D8).
AttachResult PromoteToLibrary(const std::string& library_uid, const std::string& value_uid);

}  // namespace rav

#endif  // _WIN32
