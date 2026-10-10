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
//   - Story 10-8b: the item's auto blocks (auto_detect.h) add their events from the motion
//     physics, on the item's role mapping: the physics roles are sampled once per (file, file
//     version, their bones), and one analysis runs per sensitivity set (kept for a few sets).
//   - Every edit goes through ModifyItemRules (one REAPER undo point, named after the edit,
//     that also rewrites the item's preview markers: tag_markers.h). Analyse is one edit too;
//     nothing runs it on its own. 10-6: the edit reaches the other copies of the item's pool
//     in that same undo point (item_rules.h).
//
// Main thread only (REAPER item APIs). No ImGui, no GL. No-throw.

#pragma once

#ifdef _WIN32

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "auto_detect.h"
#include "bone_events.h"
#include "event_list.h"
#include "preset_store.h"
#include "reaper_api.h"
#include "role_map_store.h"
#include "rule_record.h"
#include "tagging_signal.h"

namespace rav {

struct TaggingModel {
    MediaItem*  item = nullptr;  // the item under the playhead (null = none)
    std::string item_name;       // its take's name
    std::string path;            // its animation file
    bool        has_record = false;   // a readable record exists (none yet = the view edits an empty one;
                                      // 10-6: a linked item with none shows its pool's tagging, read through)
    bool        unreadable = false;   // a record that does not read (shown as no rules)
    ItemRules   rules;                // the saved record (no rules = default)
    // The preset field.
    bool        has_preset = false;
    std::string preset_name;      // its name on disk, the copy's name when it is gone
    bool        preset_factory = false;
    PresetState preset_state = PresetState::Unknown;
    // Story 10-3b: the preset menu and the Legacy band.
    std::string preset_id;        // the copy's id
    int         preset_version = 0;  // the copy's version
    int         kept_version = 0;    // the version Keep dismissed (0 = none)
    int         disk_version = 0;    // the preset's version on disk (0 = gone)
    bool        preset_gone = false; // the item has a preset that is no longer on disk
    // The skeleton.
    bool                     file_loaded = false;
    std::vector<std::string> bone_names;
    std::vector<int>         bone_parents;  // per bone: its parent (-1 = root), for joint angles
    std::vector<int>         role_to_bone;  // indexed by Role (-1 = no bone)
    // Story 10-3e: the user's roles (roles.txt, for every project), the non-built-in role keys
    // the item's rules read (in first-use order; a colleague's role may be in no list), and
    // the bone of each (key -> bone, -1 = none) on this skeleton: list keys and rule keys.
    std::vector<CustomRole>    custom_roles;
    std::vector<std::string>   rule_role_keys;
    std::map<std::string, int> custom_role_to_bone;
    // Story 10-3d: whether roles.txt can be read (the Skeleton & roles window says when it cannot).
    RoleMapStatus            roles_status = RoleMapStatus::Absent;
    int                      missing_count = 0;  // bone references the rules read with no bone
    std::string              missing;            // their names
    // 10-8b fb-1: a rule with a bone reference that has no bone is skipped alone (SkippedBlocks,
    // rule_record.h): the others and the auto blocks still run, live and at Commit, and its last
    // Commit's markers are kept. Per block of the rules shown: skipped, and what it misses.
    std::vector<char>        skipped;
    std::vector<std::string> skipped_missing;
    int                      skipped_count = 0;
    bool                     nothing_runs = false;  // a rule is skipped and nothing else can run:
                                                    // the item is skipped as a whole (as before fb-1)
    std::string              sample_error;       // non-empty: the bones could not be sampled
    // Detection over the rules shown (the preview while one runs, else the saved rules).
    bool           previewing = false;
    bool           detected = false;  // the trace below is for the rules shown
    DetectionTrace trace;
    ItemClipMap    map;               // clip time -> project time
    // Story 10-4: the event list over the rules shown (empty until detection ran), what Apply
    // would write for it, and whether the last Apply wrote exactly that (with this option).
    std::vector<ShownEvent>    events;
    std::vector<PlannedMarker> planned;
    // Story 10-8b: why the auto blocks found nothing ("" = they ran): the physics analysis' reason
    // (no body scale, the clip too short, the bones not sampled...).
    std::string                auto_error;
    // Story 10-8h: Detect's sensitivity search in this session (cleared on another item, file or
    // mapping), by block key ("<auto_type>:<side>", AutoSearchKey): the blocks searched, and those
    // that found no event up to 100 % (the type row says so; the note is not saved).
    // Each key maps to the sensitivity the block had after its search: a block counts as searched
    // (or "none") only while its sensitivity is still that value (an undo or an edit undoes it).
    std::map<std::string, double> auto_searched;
    std::map<std::string, double> auto_none;
    bool                       markers_up_to_date = false;
    bool                       has_previews = false;  // 10-4 fb-4: the saved record has preview markers
    // 10-6: the item's pool (read at the selection's cadence, not per frame): how many other items
    // share its tagging, and whether it was made unique (its record has a pool id of its own).
    int  pool_others = 0;
    bool pool_unique = false;
    // The selection (the footer). 10-6: Commit and Cancel also act on every other copy of the
    // selected items' pools: the counts below are over the selected items AND those copies.
    int sel_count = 0;          // selected items
    int sel_linked = 0;         // 10-6: copies of their pools that are not selected
    int sel_without_rules = 0;
    int sel_roles_skipped = 0;
    int sel_to_commit = 0;    // items Commit would change (never committed, previews, other option)
    int sel_cancellable = 0;  // 10-4 fb-4: items Cancel acts on (rules, a snapshot or previews)
};

// Story 10-4 -- one item's detection, for Apply on any selected item (not only the one shown).
struct ItemDetection {
    enum class Status { Ok, NotRav, NoRules, NoFile, RolesMissing, Failed };
    Status             status = Status::Failed;
    ItemRules          rules;   // the record as read
    ItemClipMap        map;
    std::vector<Event> events;  // the live detections (fb-1: and the kept ones of the blocks that cannot run)
    std::string        missing; // RolesMissing: their names
    // 10-8b fb-1: the rules a missing role skips (per block; Ok with some skipped = the others ran).
    std::vector<char>  skipped;
};
// Reads the item's record, binds its rules on its skeleton, samples its bones (the session's
// tracks when they fit) and runs detection, its auto blocks' too (story 10-8b: their events
// join the rules', on the item's role mapping). Never writes.
// 10-8b fb-1: a rule with an unmapped role is skipped alone (as if it were off) and an auto block
// whose part has no bone finds nothing: both keep their last Commit's markers (KeepBlockEvents,
// event_list.h), so Commit, previews, the mirror and pool follow keep them. RolesMissing only when
// a rule is skipped and nothing else can run (no rule left, no auto block on).
ItemDetection DetectItem(MediaItem* item);

// The clip map of an item (its take's start offset and rate), for a clip of `clip_len` seconds.
ItemClipMap ItemClipMapOf(MediaItem* item, double clip_len);

// A user event's values at clip time t, measured on the current item with `rules` (the rule
// `block`'s strength and speed, as detection measures them). False (0, 0) when they cannot be.
bool TaggingMeasureEvent(const ItemRules& rules, int block, double t, double* strength, double* speed);

// Reads the current item's record again and reruns detection (after Commit / Cancel, a role
// change), and the selection's counts (the footer).
void TaggingReread();

// Each frame in Tagging view (before the UI draws): `item` / `path` = the item the 3D view
// shows (null / "" = none).
void TaggingSessionFrame(MediaItem* item, const std::string& path);

const TaggingModel& GetTaggingModel();

// The rules the view shows: the live preview while a drag runs, else the saved ones.
const ItemRules& TaggingShownRules();

// A drag previews its value: the strip and the markers follow at once, nothing is written.
// `event_edit`: the preview is an event's time (its commit auto-applies), not a rule edit.
void TaggingPreview(const ItemRules& rules, bool event_edit = false);
// The drag was cancelled (Esc): back to the saved rules.
void TaggingCancelPreview();
// The drag ended: the preview is written as shown (one undo point). False without a preview.
// A preview of an event's time is written as an event correction (TaggingEditEvent).
bool TaggingCommitPreview(const char* undo_desc);

// One edit of the current item's rules: one REAPER undo point named `undo_desc`. `edit`
// gets the rules as saved (read again) and returns false to change nothing. Ends any
// preview. False (and TaggingLastError) when nothing could be written. `for_item` (optional):
// the item the edit was made on; when it is no longer the current item, nothing is written.
bool TaggingEdit(const char* undo_desc, const std::function<bool(ItemRules&)>& edit,
                 MediaItem* for_item = nullptr);

// Story 10-4 follow-up -- a manual event correction (move, add, suppress / restore / delete,
// typed time). 10-4 fb-4: exactly TaggingEdit, like every other gesture: the record and the
// item's preview markers in one undo point; the committed markers wait for Commit (fb-1's
// immediate rewrite of the committed markers is replaced).
bool TaggingEditEvent(const char* undo_desc, const std::function<bool(ItemRules&)>& edit,
                      MediaItem* for_item = nullptr);

// Analyse: proposes the thresholds from the clip (Fixed conditions keep theirs). One undo
// point. False when the item cannot be analysed (missing roles, no tracks).
bool TaggingAnalyse();

// Story 10-8b -- Detect in the Auto detection section: the item's auto blocks become these
// settings (ApplyAutoSettings: ticked types added, unticked ones removed with their events, steps
// switched between separate and combined), the event list remapped. One undo point ("RAV: Detect
// auto events"); nothing is written when the blocks would not change.
// Story 10-8h: in the same undo point, every searchable block with no detected event gets the
// lowest sensitivity that finds one (SearchAutoSensitivity, auto_detect.h; raised: sens_from
// set), and the model's auto_searched / auto_none are updated. Written also when only a
// sensitivity was raised.
bool TaggingDetectAuto(const AutoSettings& settings);
// Story 10-8i -- "Is looping" in the Auto detection section: the item's ClipSettings::loop
// (rule_record.h) set to `on`, one undo point ("RAV: Set looping"); the auto events are read
// again at once (no Detect needed). Nothing is written when it already is `on`, or when
// `for_item` is no longer the current item. A change clears
// the model's auto_searched / auto_none (they were read on the other analysis).
bool TaggingSetLooping(bool on, MediaItem* for_item = nullptr);
// Story 10-8h: an auto block's key in TaggingModel::auto_searched / auto_none, and whether the
// block is in that map at its current sensitivity.
std::string AutoSearchKey(const Block& b);
bool        AutoSearchedAt(const std::map<std::string, double>& in, const Block& b);

// Loads a preset on the item (SetUpPresetOnItem): one undo point.
bool TaggingLoadPreset(const std::string& preset_id);

// Story 10-3b -- the preset menu. Save (`preset_id` = the preset overwritten) / Save as
// (`preset_id` empty, a new preset named `name`): the file is written, then the item adopts it
// (SaveItemAsPreset, one undo point). `saved_name` gets the preset's name, `err` the reason
// it failed (the menu's status line shows it; TaggingLastError is left alone).
bool TaggingSavePreset(const std::string& preset_id, const std::string& name, std::string* saved_name,
                       std::string* err);
// The Legacy band: Update ("RAV: Update rules from preset") and Keep ("RAV: Keep current
// rules"), one undo point each. Errors go to TaggingLastError.
bool TaggingUpdatePreset();
bool TaggingKeepPreset();
// The preset files changed on disk (rename, delete, import, a save): the preset field's name,
// state and versions are read again (no detection).
void TaggingPresetFilesChanged();

// 10-6 -- "Make unique" on the item (MakeItemUnique, one undo point): it no longer shares its
// tagging with the other copies of its file. `for_item`: the item it was clicked on (nothing
// is written when it is no longer the current item). Errors go to TaggingLastError.
bool TaggingMakeUnique(MediaItem* for_item);

// The last write's error ("" when it went through), shown inline.
const std::string& TaggingLastError();
void TaggingClearError();

// The viewer closes: forget the item, the tracks and the preview.
void TaggingSessionReset();

}  // namespace rav

#endif  // _WIN32
