// SPDX-License-Identifier: MIT
//
// The Tagging view's model (Epic 10, story 10-3): the item under the playhead, its rules,
// its skeleton binding, its bone tracks and the detection trace the strip draws.
//
//   - The item is the one RAV view shows (the viewer passes it each frame).
//   - Its record (item_rules.h) is read again when the item changes, when REAPER's project
//     state changes (an undo, another script) and at most once a second otherwise.
//   - The bones the rules read are sampled once per (file, file version, bone set), at the
//     detection rate (240 Hz), from the parsed file (bone_sampling.h).
//   - Detection (DetectTrace, bone_events.h) reruns when the rules shown change: the saved
//     rules, or a live preview while a drag runs (the preview is never written).
//   - Every edit goes through ModifyItemRules (one REAPER undo point, named after the edit).
//     Analyse is one edit too; nothing runs it on its own.
//
// Main thread only (REAPER item APIs). No ImGui, no GL. No-throw.

#pragma once

#ifdef _WIN32

#include <functional>
#include <string>
#include <vector>

#include "bone_events.h"
#include "preset_store.h"
#include "reaper_api.h"
#include "rule_record.h"
#include "tagging_signal.h"

namespace rav {

struct TaggingModel {
    MediaItem*  item = nullptr;  // the item under the playhead (null = none)
    std::string item_name;       // its take's name
    std::string path;            // its animation file
    bool        has_rules = false;    // a readable record with at least one rule or a preset
    bool        unreadable = false;   // a record that does not read (shown as no rules)
    ItemRules   rules;                // the saved record (no rules = default)
    // The preset field.
    bool        has_preset = false;
    std::string preset_name;
    bool        preset_factory = false;
    PresetState preset_state = PresetState::Unknown;
    // The skeleton.
    bool                     file_loaded = false;
    std::vector<std::string> bone_names;
    std::vector<int>         role_to_bone;  // indexed by Role (-1 = no bone)
    int                      missing_count = 0;  // bone references the rules read with no bone
    std::string              missing;            // their names
    std::string              sample_error;       // non-empty: the bones could not be sampled
    // Detection over the rules shown (the preview while one runs, else the saved rules).
    bool           previewing = false;
    bool           detected = false;  // the trace below is for the rules shown
    DetectionTrace trace;
    ItemClipMap    map;               // clip time -> project time
    // The selection (the footer).
    int sel_count = 0;
    int sel_without_rules = 0;
    int sel_roles_skipped = 0;
};

// Each frame in Tagging view (before the UI draws): `item` / `path` = the item the 3D view
// shows (null / "" = none).
void TaggingSessionFrame(MediaItem* item, const std::string& path);

const TaggingModel& GetTaggingModel();

// The rules the view shows: the live preview while a drag runs, else the saved ones.
const ItemRules& TaggingShownRules();

// A drag previews its value: the strip and the markers follow at once, nothing is written.
void TaggingPreview(const ItemRules& rules);
// The drag was cancelled (Esc): back to the saved rules.
void TaggingCancelPreview();
// The drag ended: the preview is written as shown (one undo point). False without a preview.
bool TaggingCommitPreview(const char* undo_desc);

// One edit of the current item's rules: one REAPER undo point named `undo_desc`. `edit`
// gets the rules as saved (read again) and returns false to change nothing. Ends any
// preview. False (and TaggingLastError) when nothing could be written. `for_item` (optional):
// the item the edit was made on; when it is no longer the current item, nothing is written.
bool TaggingEdit(const char* undo_desc, const std::function<bool(ItemRules&)>& edit,
                 MediaItem* for_item = nullptr);

// Analyse: proposes the thresholds from the clip (Fixed conditions keep theirs). One undo
// point. False when the item cannot be analysed (missing roles, no tracks).
bool TaggingAnalyse();

// Loads a preset on the item (SetUpPresetOnItem): one undo point.
bool TaggingLoadPreset(const std::string& preset_id);

// The last write's error ("" when it went through), shown inline.
const std::string& TaggingLastError();
void TaggingClearError();

// The viewer closes: forget the item, the tracks and the preview.
void TaggingSessionReset();

}  // namespace rav

#endif  // _WIN32
