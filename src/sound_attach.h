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

// 13-68: one eligible selected item (ReadSelectedSounds), and the selection read by the attach rules
// (no RAV item, no folder item, no item RAV placed; an attached item is eligible): `skip_note` as above.
struct SelectedSound {
    MediaItem* item = nullptr;
    AttachItem facts;
    bool       tagged = false;  // carries RAV_SND (eligible: attached)
    PlacedTag  tag;
    bool       folder = false;  // a folder item (an NVK pile; only with `allow_folders`)
};
// `allow_folders` (13-68 v2, Spread selection only): a folder item is eligible too (its pile is one Sound).
void ReadSelectedSounds(std::vector<SelectedSound>* eligible, int* skipped, std::string* skip_note,
                        bool allow_folders = false);
// 13-68: the rid of every rule of the animation item, giving the rules ids first (inside the
// caller's undo scope). False (and `err`) when the rules cannot be read or written.
bool ItemRuleIds(MediaItem* anim, std::vector<std::string>* rids, std::string* err);

// 13-68 v2: an item as a library Sound, as Promote reads it: its data (an item source; 2+ takes = its
// takes as variations, Takes mode) and its name (its active take's, "Sound" when none).
SoundData   ItemSoundData(MediaItem* item);
std::string ItemSoundName(MediaItem* item);

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
