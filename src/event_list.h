// SPDX-License-Identifier: MIT
//
// The item's event list (Epic 10, story 10-4): what the Tagging view shows and what Apply
// writes, from the live detections and the record's entries (rule_record.h).
//
//   - Displayed list = each rule's live detections, each `Suppressed` when a suppression of
//     that rule lies within +-30 ms (inclusive), else `Detected`; plus the suppressions with
//     no detection of their rule that near (`Orphan`); plus the user's own events (`User`,
//     fixed in time: detection never moves them).
//   - Apply writes the Detected and User events of the rules that are on, at their project
//     time on the item's FIRST pass of the clip only (RAV plays the clip once then holds its
//     last frame), inside the item's bounds. 13-56: on a looped item the marker mirror adds a
//     project marker on every other loop pass (PlanLoopEchoes).
//   - Deleting or duplicating a rule remaps the entries' rule indices (a deleted rule's user
//     events and suppressions go with it).
//   - The event list belongs to the take, like REAPER's take markers and P_EXT: a changed
//     source path (relinked, moved, replaced, re-exported) never clears it. Loading a preset
//     does (ClearCorrections).
//   - "Markers up to date" = the last Apply ran with the same option and wrote the same
//     markers (a signature of the planned markers; 10-4b: independent of where the item sits).
//   - 10-4b: the project markers follow their item (MirrorPlan, driven by tag_markers.h).
//   - 10-4c: a REAPER-side drag, delete or rename of RAV's markers changes the event list
//     (MirrorPlan's user steps, ApplyMarkerEdits, MatchTakeRefs).
//
// Pure C++17: no REAPER, no ImGui, no Windows. Host-tested (tests/event_list_test.cpp).

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "bone_events.h"
#include "rule_record.h"
#include "tagging_signal.h"

namespace rav {

constexpr double kSuppressWindowS = 0.030;  // a suppression masks detections this close (inclusive)
constexpr double kMarkerTwinTolS = 0.001;   // a foreign marker this close with the same name = already present

enum class ShownKind { Detected, Suppressed, Orphan, User };

struct ShownEvent {
    double    t = 0.0;  // clip seconds
    int       block = 0;
    ShownKind kind = ShownKind::Detected;
    double    strength = 0.0;  // Detected / Suppressed: detection's; User: as stored
    double    speed = 0.0;
    int       entry = -1;      // Suppressed: the suppression; Orphan / User: the entry (index into ItemRules::events)
    int       detection = -1;  // Detected / Suppressed: index into the detections given
};

// The displayed list, sorted by time (then rule). `detections` = the live detections (any
// order, Event::block = the rule), `entries` = the record's. Entries of an unknown kind, of
// the Detected kind (Apply's snapshot) or of a rule out of [0, n_blocks) are not shown.
std::vector<ShownEvent> BuildEventList(const std::vector<Event>& detections, const std::vector<EventEntry>& entries,
                                       size_t n_blocks);

// New entries (every optional field written).
EventEntry MakeUserEvent(int block, double t, double strength, double speed);
EventEntry MakeSuppression(int block, double t);

// ---- 10-8b fb-1: a block that cannot run keeps its last Commit ---------------------------------
//
// A rule skipped for a missing role (SkippedBlocks, rule_record.h), or an auto block whose own
// part has no bone (AutoBlockPartMissing, auto_detect.h), keeps the markers the last Commit wrote
// for it until the bone is there: the record's Detected entries of that block (Commit's record of
// the detections it wrote: RecordApplied) stand in for its detections, with their stored values;
// its user events stay in the record as they are. A block never committed has none. So the event
// list, PlanMarkers, previews and Commit (which records them again) keep its markers as they were.

// `detections` with every event of a kept block (`keep`, per block; shorter = not kept) replaced
// by the record's Detected entries of that block (Event::marker = the block's marker). Sorted by
// time (then block). Nothing kept: unchanged.
void KeepBlockEvents(std::vector<Event>& detections, const std::vector<EventEntry>& entries,
                     const std::vector<Block>& blocks, const std::vector<char>& keep);

// ---- Rule delete / duplicate ------------------------------------------------------------------

// old index -> new index (-1 = gone) when rule `deleted` of n is deleted.
std::vector<int> BlockMapForDelete(size_t n_blocks, int deleted);
// ... when a rule is inserted at `at` (the rules from `at` on move one down).
std::vector<int> BlockMapForInsert(size_t n_blocks, int at);
// Applies the map to the entries of known kinds: an entry of a gone rule is erased. An index
// outside the map is left as it is.
void RemapEventBlocks(std::vector<EventEntry>& entries, const std::vector<int>& old_to_new);

// ---- The first pass of the clip on the timeline ------------------------------------------------

// The item's first pass of the clip (clip time 0 .. clip_len), clipped to the item. False
// when the item shows nothing of it.
bool FirstClipPass(const ItemClipMap& m, ClipPass* out);
// Clip time -> project time on the first pass, inside the item's bounds. False outside.
bool FirstPassProjectTime(const ItemClipMap& m, double clip_t, double* project_t);
// Project time -> clip time on the first pass. False outside it.
bool FirstPassClipTime(const ItemClipMap& m, double project_t, double* clip_t);

// ---- 13-56: every loop pass of a looped item -----------------------------------------------------

// The id of a pass of the clip (ClipPasses): the clip loop it plays, its source time being
// id * clip_len + clip time. It stays the same when the item moves, is trimmed at its start or
// changes rate. 0 when the item does not loop (and for the first pass, FirstClipPass).
long long ClipPassId(const ItemClipMap& m, const ClipPass& p);
// Where clip time `clip_t` shows on the timeline, one per pass inside the item, in time order
// (ClipToProjectTimes' test): the pass id and the project time.
struct LoopPassTime {
    long long id = 0;
    double    project_t = 0.0;
};
// Review 13-56: only the item's first kMaxLoopPasses passes (time order) are looped over, so a long
// stretch does not write thousands of markers and sounds; `capped` (optional) is set when the item
// shows more.
constexpr size_t kMaxLoopPasses = 256;
std::vector<LoopPassTime> LoopPassTimes(const ItemClipMap& m, double clip_t, bool* capped = nullptr);
// Clip time -> project time on pass `id` (0: FirstPassProjectTime). False when that pass does not
// show it (or the item does not loop and id is not 0, or the pass is past kMaxLoopPasses). Read
// from the id (no walk over the passes).
bool PassProjectTime(const ItemClipMap& m, double clip_t, long long id, double* project_t);
// Project time -> clip time on pass `id` (0: FirstPassClipTime). False outside that pass (or past
// kMaxLoopPasses).
bool PassClipTime(const ItemClipMap& m, double project_t, long long id, double* clip_t);

// ---- Apply (Commit since 10-4 fb-4) ------------------------------------------------------------

struct PlannedMarker {
    double      clip_t = 0.0;     // = the take marker's source position
    double      project_t = 0.0;  // the project marker's position (meaningful only when in_item)
    int         block = 0;
    std::string name;             // the rule's marker name, on one line, trimmed
    uint32_t    color = 0;        // the rule's colour (Block::color: 0 = none)
    bool        user = false;     // a user event (else a detection)
    double      strength = 0.0;
    double      speed = 0.0;
    bool        in_item = true;   // 10-4b: the event shows on the item's first pass (else no project marker is written)
};

// What Apply writes for this list: its Detected and User events whose rule exists and is on,
// sorted by time. 10-4b: an event outside the item's first pass is kept with `in_item` false
// (it gets a take marker, in source time, and a hidden project ref the mirror shows again when
// the item is extended).
std::vector<PlannedMarker> PlanMarkers(const std::vector<ShownEvent>& list, const std::vector<Block>& blocks,
                                       const ItemClipMap& map);

// A stable signature of the planned markers for a mode: every planned marker (inside the item
// or not), by rule, colour, clip time and name. 10-4b: it no longer depends on where the item
// sits (in any mode), so a moved or trimmed item stays "markers up to date": the mirror hides and
// shows the project markers, and take markers (written for every planned marker, in source
// time) show again by themselves when the item is extended. 16 hex digits.
std::string MarkerSignature(const std::vector<PlannedMarker>& planned, MarkerMode mode);

// True when the last Apply wrote these markers with this option.
bool MarkersUpToDate(const ItemRules& rules, const std::vector<PlannedMarker>& planned, MarkerMode mode);

// Apply's record: the Detected entries are replaced by the detections written (their values
// kept), `applied` set. The owned-marker lists are the caller's.
void RecordApplied(ItemRules& rules, const std::vector<PlannedMarker>& planned, MarkerMode mode);

// 10-4b: what Commit / a preview rewrite does for one planned marker (the REAPER calls are the
// writer's: tag_markers.cpp WritePlan).
struct MarkerWrite {
    size_t           planned = 0;     // index into the plan
    bool             take = false;    // write a take marker at clip_t (inside the item or not: source time)
    bool             project = false; // write a project marker at project_t (inside the item only)
    bool             hidden = false;  // record `ref` as a hidden project ref (no marker, no GUID)
    ProjectMarkerRef ref;             // the project ref's fields (c, colour, name; guid filled by the writer).
                                      // For a hidden ref, `t` is the clip time extrapolated onto the
                                      // timeline outside the item: informative only, never placed.
};
// One entry per planned marker that writes or records something. Every planned marker gets a
// take marker when they are wanted (REAPER hides one outside the item). An event outside the
// item writes no project marker; with project markers wanted it is recorded hidden, once per
// (clip time, name) like the written ones.
std::vector<MarkerWrite> PlanMarkerWrites(const std::vector<PlannedMarker>& plan, bool want_take, bool want_project);

// A marker already there (not RAV's): its position and name.
struct ExistingMarker {
    double      t = 0.0;
    std::string name;
};
// The index of a marker with exactly this name within +-tol of t, or -1.
int FindTwinMarker(const std::vector<ExistingMarker>& existing, double t, const std::string& name,
                   double tol = kMarkerTwinTolS);

// ---- 10-4 fb-4: preview markers, Commit and Cancel ----------------------------------------------
//
// Preview markers show the tool's current result on the REAPER timeline next to the committed
// ones (the `applied` / `tmarker` / `pmarker` of the record): the planned markers, named
// "<marker name> - Preview", in the rule's colour darkened ~50 % toward black (REAPER markers
// have no alpha). There is none while the current result equals the committed one.

constexpr char kPreviewSuffix[] = " - Preview";

// "<name> - Preview".
std::string PreviewMarkerName(const std::string& name);
// Block::color darkened ~50 % toward black (0x1000000 | 0xRRGGBB). A rule without colour (0)
// gets a dark grey, so its previews still stand apart from REAPER's default colour.
uint32_t PreviewColor(uint32_t color);
// The planned markers as previews (names and colours above, times unchanged).
std::vector<PlannedMarker> PreviewPlan(const std::vector<PlannedMarker>& planned);
// True when previews must show: the current result is not the committed one (no Commit yet,
// another option, other markers). An empty result never committed needs none.
bool PreviewNeeded(const ItemRules& rules, const std::vector<PlannedMarker>& planned, MarkerMode mode);
// The record of previews written for `planned` (the owned-marker lists are the caller's).
void RecordPreviewed(ItemRules& rules, const std::vector<PlannedMarker>& planned, MarkerMode mode);
// No preview any more: the preview state and the preview-marker lists are cleared.
void ClearPreviewed(ItemRules& rules);
// True when the record has preview markers (or their state).
bool HasPreviews(const ItemRules& rules);

// An item "has rules" for the Cancel snapshot: a rule or a preset.
bool HasRulesForSnapshot(const ItemRules& rules);
// Cancel: the record as of the last Commit (`snapshot`), with the committed markers RAV owns NOW
// (`current`'s tmarkers / pmarkers: the timeline's truth), `current`'s owner and pool (10-6), and
// no preview.
ItemRules RestoreCommitted(const ItemRules& snapshot, const ItemRules& current);

// Cancel's target for one item: the snapshot (when it parses) restored with RestoreCommitted,
// else the current record without its previews. False (`next` untouched) when Cancel would
// change nothing (no previews and the same record text as `cur_raw`).
bool CancelTarget(const ItemRules& cur, const std::string& cur_raw, const std::string* snapshot_text, ItemRules* next);

// The first Cancel snapshot to store after a gesture ("" = write nothing): never over an
// existing one; the record before the gesture when it already had rules, else the record
// after it when it has rules now.
std::string FirstCommittedSnapshot(bool has_snapshot, bool before_valid, const ItemRules& before,
                                   const std::string& before_raw, const ItemRules& after);

// Commit's record on one item: the committed markers RAV now owns (`own_take` / `own_project`;
// appended to the old lists when that kind could not be deleted), the applied state of
// `planned`, no preview state; the preview refs of a kind that could not be deleted stay RAV's.
void ComposeCommittedRecord(ItemRules& rec, const std::vector<TakeMarkerRef>& own_take,
                            const std::vector<ProjectMarkerRef>& own_project, bool keep_take_refs,
                            bool keep_project_refs, const std::vector<PlannedMarker>& planned, MarkerMode mode);

// ---- 10-11: an item with no rule left, and a project marker two items want ---------------------
//
// An item whose rules were all deleted still owns the markers its last Commit wrote: Commit runs
// on it with an empty plan, which deletes them (and records none).
//
// Two items may plan the same project marker (same name, same time within kMarkerTwinTolS:
// stacked character and weapon items). REAPER gets one marker, owned (in its record) by the item
// that wrote it; the other's Commit found it already present. So that rewriting the owner never
// takes the marker from the other one, an owned project marker about to be deleted (Commit, Clear
// item data) is handed over instead to another item that still wants it: its last Commit planned
// a twin inside the item and it owns no twin itself. Nothing is handed over when the marker is
// being written again anyway (a twin in this Commit's new plans). One owner per GUID, as the
// mirror expects.

// True when the record owns committed markers (take or project).
bool HasCommittedMarkers(const ItemRules& rules);
// True when Commit runs on an item with no rule: it owns committed markers to clear.
bool CommitClearsEmptyItem(const ItemRules& rules);

// The project markers the record's last Commit planned inside the item (its Detected entries and
// user events, as RecordApplied left them, on `map`). None when it has no Commit or its option
// wrote take markers only.
std::vector<PlannedMarker> CommittedProjectWants(const ItemRules& rules, const ItemClipMap& map);

// Another item that may take over a marker.
struct MarkerClaimant {
    std::vector<PlannedMarker>    wants;  // CommittedProjectWants
    std::vector<ProjectMarkerRef> owns;   // its committed project refs (grown by a hand-over)
};

// For each `leaving` ref (an owned committed project marker about to be deleted): -1 = delete it
// (no GUID, no name, written again by a twin in `rewritten`, or no claimant), else the index of
// the claimant that takes it, the first in order; (*given)[i] is then the ref to add to that
// claimant's record (the marker's GUID and place, the claimant's clip time, colour and name).
std::vector<int> HandOverProjectMarkers(const std::vector<ProjectMarkerRef>& leaving,
                                        const std::vector<PlannedMarker>& rewritten,
                                        std::vector<MarkerClaimant>& claimants, std::vector<ProjectMarkerRef>* given);

// ---- 10-4b: the marker mirror ----------------------------------------------------------------------
//
// RAV's project markers (committed and previews) follow their item: a main-thread timer
// (tag_markers.h) places each one from its event's clip time (ProjectMarkerRef::c) and the
// item's current position, start offset, rate and length (FirstPassProjectTime, Commit's rule;
// 13-56: a loop echo on its own pass, PassProjectTime).
//
// 10-4c: two-way. State-based, not cache-based: each ref says where RAV last put its marker
// (`t`) and where its event belongs (`c`). A marker at its expected place is in place (a ripple:
// only `t` is updated); one still where RAV put it while the expected place moved = its item
// changed (Move / Hide); one at neither = the user dragged it. A missing marker the mirror did
// not delete = the user deleted it; another name = the user renamed it (the marker becomes theirs).

constexpr double kMirrorPosTolS = 1e-5;  // a marker this close to a time is at it

enum class MirrorAction {
    Keep,    // nothing to do
    Move,    // move the marker to new_t
    Hide,    // its event is outside the item: the ref's GUID cleared (the marker deleted when delete_old)
    Show,    // a new marker at new_t (the ref takes its GUID): a hidden event back inside the item, a
             // marker the mirror itself deleted, or a copy's own marker
    Retime,  // 10-4c: the marker is at its expected place but not where RAV last put it (ripple):
             // the ref's `t` becomes new_t, the marker is not touched
    UserDrag,    // 10-4c: the user dragged it inside the item: its event moves to new_c (marker at new_t)
    UserDelete,  // 10-4c: the user deleted it
    UserDisown,  // 10-4c: the user renamed it, or dragged it outside the item: it is theirs now
};

struct MirrorStep {
    size_t       ref = 0;  // index into the refs given
    MirrorAction action = MirrorAction::Keep;
    double       new_t = 0.0;        // Move / Show / Retime / UserDrag: the project time
    double       new_c = 0.0;        // UserDrag: the clip time it was dropped at
    bool         delete_old = false;  // Hide: delete the marker under the ref's GUID
};

// What the timeline holds for one ref.
struct MirrorRefState {
    bool        exists = false;    // a project marker with the ref's GUID is there
    double      now_t = 0.0;       // its position, when it exists
    bool        restore = false;   // its GUID is missing because the mirror deleted it: bring it back
    std::string name;              // 10-4c: its name, when it exists (has_name)
    bool        has_name = false;
};

// One step per ref (same order). `map_changed`: the item's clip map differs from the one the
// mirror saw on its last scan (false on first sight): a marker at neither its expected place nor
// where RAV put it is then placed from the item (the item wins), never read as a user drag. A
// missing marker whose event is outside the item is hidden, not read as a delete. `copy`: the record is a copy of another item's (duplicate,
// paste, right part of a split): every ref with a clip time gets a fresh marker of its own (Show),
// or is hidden without touching the original's (Hide, delete_old false; a ref without a clip time
// on a copy too: the marker is the original's); a copy never reads user edits. Otherwise a ref
// without a clip time (older record) is always kept, and one without a name is never read as a
// user edit (a missing or dragged marker is then kept as it is).
std::vector<MirrorStep> MirrorPlan(const std::vector<ProjectMarkerRef>& refs, const std::vector<MirrorRefState>& state,
                                   const ItemClipMap& map, bool map_changed, bool copy);

// ---- 13-56: project markers on every loop pass ----------------------------------------------------
//
// On a looped item each first-pass ref (pass 0) with a clip time and a name has an echo (the same
// clip time, name and colour, `pass` = the pass id) on every other loop pass that shows its clip
// time; Commit writes the first pass only, the mirror makes and drops the echoes (no undo point,
// like its other writes). A not-looped item has none. Take markers are not echoed: REAPER shows a
// take marker on every loop of its item by itself.

constexpr double kEchoMatchTolS = 1e-6;  // an echo this close to its first-pass ref's clip time is its
// Review 13-56: at most this many echoes per ref list (committed, previews) of one item, on top of
// kMaxLoopPasses: the first refs in list order on the first passes get theirs.
constexpr size_t kMaxLoopEchoes = 4096;

// An echo's place: its pass, its name and its clip time rounded to kEchoMatchTolS. A first-pass ref
// wants one echo per key; an existing echo is kept when its key is wanted (and not taken already).
struct EchoKey {
    long long   pass = 0;
    std::string name;
    long long   c = 0;
    bool operator<(const EchoKey& o) const
    {
        if (pass != o.pass) return pass < o.pass;
        if (c != o.c) return c < o.c;
        return name < o.name;
    }
};
EchoKey EchoKeyOf(long long pass, const std::string& name, double c);

struct EchoStep {
    bool             drop = false;  // drop refs[ref] (its marker deleted when it has a GUID)
    size_t           ref = 0;
    ProjectMarkerRef add;           // !drop: a new echo, its marker to write at add.t (GUID by the writer)
};
// What the echoes of `refs` (one list: committed or previews) must do on `map`: an echo whose first-
// pass ref is gone, whose pass no longer shows its clip time, without a GUID (hidden, or a copy's
// that could not be made), or a second one for the same place, is dropped; a missing one is added.
// Drops first (descending ref index), then adds in ref order then pass order. Empty when all are
// in place (always, for a not-looped item without echoes). `capped` (optional) is set when the item
// shows more passes than kMaxLoopPasses or wants more echoes than kMaxLoopEchoes (the rest not echoed).
std::vector<EchoStep> PlanLoopEchoes(const std::vector<ProjectMarkerRef>& refs, const ItemClipMap& map,
                                     bool* capped = nullptr);

// ---- 10-4c: REAPER-side edits of RAV's markers -> the event list ---------------------------------

enum class MarkerEditKind {
    Drag,    // moved to new_c: a detection becomes a user event there (suppressed where it was), a
             // user event moves
    Delete,  // a detection is suppressed, a user event removed
    Disown,  // renamed (or dragged outside the item): as Delete for the event list; the marker is
             // the user's from now on (the caller drops its ref)
};

struct MarkerEdit {
    MarkerEditKind kind = MarkerEditKind::Drag;
    double         c = 0.0;      // the event's clip time (the ref's)
    double         new_c = 0.0;  // Drag: the clip time it was dropped at
    std::string    name;         // the marker's name as RAV wrote it
    uint32_t       color = 0;    // ... its colour (has_color)
    bool           has_color = false;
    bool           preview = false;  // a preview marker ("<marker> - Preview", colour darkened)
};

// The rule a marker belongs to (by its name, then its colour; a rule with an event at its clip
// time first), or -1.
int MarkerEditBlock(const ItemRules& rules, const MarkerEdit& e);

// The record's event list with the edits applied, with the Tagging view's Move / Delete
// semantics (a detection: suppressed, plus a user event on a drag; a user event: moved or
// removed) but the values are KEPT, never re-measured (spec decision): a dragged detection keeps
// its strength / speed (the applied snapshot's, else the nearest live detection's within 1 ms from
// `detections`, else 0); a dragged user event keeps its values. An
// edit whose rule is not found, or a second edit of the same event, changes nothing. `blocks`
// (optional) gets each edit's rule (-1 = ignored). Only `events` changes.
ItemRules ApplyMarkerEdits(const ItemRules& rules, const std::vector<MarkerEdit>& edits,
                           const std::vector<Event>* detections = nullptr, std::vector<int>* blocks = nullptr);

// Both: the other marker of one event. The committed take ref at clip time c (+-tol) with this
// name, or the project ref with a clip time at c (+-tol) and this name (13-56: on the first pass,
// never a loop echo); -1 when none. A ref whose
// `skip` entry is non-zero (already handled or dropped this scan) is passed over; `skip` may be
// shorter than the refs (missing entries = not skipped).
int FindTakeTwin(const std::vector<TakeMarkerRef>& tmarkers, double c, const std::string& name, double tol,
                 const std::vector<char>& skip);
int FindProjectTwin(const std::vector<ProjectMarkerRef>& pmarkers, double c, const std::string& name, double tol,
                    const std::vector<char>& skip);

// Take markers have no GUID: RAV's are recognised by time and name (kOwnTakeTolS in
// tag_markers.cpp). A ref whose marker is not there is matched by elimination among the take's
// markers no ref matches: one at the same time with another name -> Rename; else the nearest one
// with the same name elsewhere -> Drag; else -> Delete.
enum class TakeRefFate { InPlace, Drag, Rename, Delete };
struct TakeRefMatch {
    TakeRefFate fate = TakeRefFate::InPlace;
    int         marker = -1;  // InPlace / Drag / Rename: the index into the markers given
};
std::vector<TakeRefMatch> MatchTakeRefs(const std::vector<TakeMarkerRef>& refs,
                                        const std::vector<ExistingMarker>& markers, double tol);

// True when the mirror manages this record: a project marker ref with a clip time (an older
// record behaves as before until its next Commit).
bool MirrorManaged(const ItemRules& rules);

// The item that owns the record's markers ("" = none): the `applied` line's, else the `preview`
// line's.
std::string RecordOwner(const ItemRules& rules);
// Sets the owner on the record's `applied` and `preview` lines (those it has). False when it
// has neither (nowhere to keep it).
bool SetRecordOwner(ItemRules& rules, const std::string& item_guid);

enum class MirrorOwnership {
    Own,    // the record's owner is this item
    Adopt,  // no owner, and no other item claims its markers: the item becomes the owner
    Copy,   // another item's record: fresh markers, the item becomes the owner
};
// `claimed_elsewhere`: another item already owns one of the record's marker GUIDs.
MirrorOwnership DecideMirrorOwnership(const std::string& owner, const std::string& item_guid, bool claimed_elsewhere);

// One RAV item as the mirror reads it on a scan.
struct MirrorOwnerInput {
    std::string              item;     // its GUID
    std::string              owner;    // its record's owner ("" = none)
    bool                     managed = false;
    std::vector<std::string> guids;    // the project-marker GUIDs its record lists
};
struct MirrorOwnerResult {
    std::vector<MirrorOwnership>       own;      // per input (an unmanaged item: Own, ignored)
    std::map<std::string, std::string> claimed;  // marker GUID -> the item that keeps it
};
// The scan's ownership, in input order. A self-owner keeps its markers unless one of them was
// held on the last scan (`prev_owners`, marker GUID -> item GUID) by another self-owner read
// now, or was already claimed by an earlier self-owner of this scan (a gesture on a copy before
// the mirror saw it): then it is a copy. A record of another owner is a copy. A record without
// owner adopts its item unless one of its markers is claimed now or was held on the last scan
// by another item still present (`present`), else it is a copy.
MirrorOwnerResult ArbitrateMirrorOwnership(const std::vector<MirrorOwnerInput>& items,
                                           const std::map<std::string, std::string>& prev_owners,
                                           const std::vector<std::string>& present);

// After a scan: one item, its markers' GUIDs now, and whether it holds them (Own / Adopt, or a
// copy whose fresh markers were recorded). A copy that could not refresh still lists the
// original's GUIDs: it holds none.
struct MirrorHeld {
    std::string              item;
    bool                     managed = false;
    bool                     holds = false;
    std::vector<std::string> guids;
};
// The next scan's marker GUID -> item map: the markers held now, plus the last scan's entries of
// an item present but not read now (another active take). `read_items` = the items read now.
std::map<std::string, std::string> NextMirrorOwners(const std::vector<MirrorHeld>& held,
                                                    const std::map<std::string, std::string>& prev_owners,
                                                    const std::vector<std::string>& present,
                                                    const std::vector<std::string>& read_items);
// The GUIDs claimed now (for MirrorOrphans): those of the items that hold them.
std::vector<std::string> MirrorClaimedNow(const std::vector<MirrorHeld>& held);

// The marker GUIDs whose item is gone (or no longer lists them): in `prev` (marker GUID -> item
// GUID, the last tick's), claimed by no item now (`claimed`), and whose item is no longer in the
// project or is read now (`read_items`; an item present but not read, e.g. another active take,
// keeps its markers). Sorted.
std::vector<std::string> MirrorOrphans(const std::vector<std::pair<std::string, std::string>>& prev,
                                       const std::vector<std::string>& claimed,
                                       const std::vector<std::string>& present_items,
                                       const std::vector<std::string>& read_items);

// ---- 10-6: pooled copies (Cancel and the mirror on a pool's copies) -------------------------------

// Cancel on one pool: one copy as Cancel reads it.
struct CancelCopy {
    bool        shown = false;     // the copy the Tagging view shows
    bool        selected = false;
    bool        readable = false;  // it has a record that reads (`rules`, `raw`)
    ItemRules   rules;
    std::string raw;
    bool        has_snap = false;  // it has a Cancel snapshot (`snap_text`)
    std::string snap_text;
};
// True when Cancel can act on the copy: a record that reads, with rules, a snapshot or previews.
bool CancelCopyCancellable(const CancelCopy& c);
struct CancelWrite {
    size_t    copy = 0;  // index into the copies
    ItemRules next;
};
struct PoolCancelPlan {
    int                      from = -1;          // the copy it runs from (-1: none can, nothing is written)
    std::vector<CancelWrite> writes;             // the run-from copy first (when it changes), then the others
    int                      without_rules = 0;  // selected copies counted "without rules"
};
// One pool's Cancel (`copies`: the pool's items, the selected ones first). It runs from the copy
// the Tagging view shows, else the first selected one, else the first one, that can
// (CancelCopyCancellable): its CancelTarget (its record as it is when it has nothing to cancel) is
// the target. Every other copy with a readable record gets the target's content (CopyPoolContent)
// and no preview (ClearPreviewed; its own bookkeeping kept), unless that changes nothing (no
// preview and the same text as `raw`). Selected copies without a readable record, or every
// selected copy when none can cancel, count as without rules.
PoolCancelPlan PlanPoolCancel(const std::vector<CancelCopy>& copies);

// The REAPER-side edits the mirror read on one copy in one scan (MirrorOwnItem: its record and its
// Cancel snapshot already took them).
struct CopyEdits {
    std::vector<MarkerEdit> all;        // the committed markers' edits, then the previews'
    std::vector<MarkerEdit> committed;  // the committed markers' edits
    bool                    det_ok = false;
    std::vector<Event>      detections;  // its live detections (det_ok): the pool's, same file and rules
};
// The pool's content after edits read on several of its copies in one scan: `first` (the record of
// the first copy, written with its own edits), each of `others` applied to its events in turn.
ItemRules MergePoolEdits(const ItemRules& first, const std::vector<CopyEdits>& others);
// The committed edits a copy's Cancel snapshot still has to take: those read on the OTHER copies
// of `edited` (`own`: the index of its own edits there, -1 when none were read on it: all of them).
std::vector<MarkerEdit> SnapshotEditsFor(const std::vector<CopyEdits>& edited, int own);
// A copy's record following its pool's content (the mirror): `cur` with `content`'s content.
// `replaced`: its committed markers were re-placed for `plan` (`own_take` / `own_project` written,
// `keep_*` as ComposeCommittedRecord): the committed record of them, no preview, and the events
// the pool's as they are (a Commit's record of the detections is not redone). `self_guid`
// non-empty: the record's owner becomes it.
ItemRules FollowerRecord(const ItemRules& cur, const ItemRules& content, bool replaced,
                         const std::vector<TakeMarkerRef>& own_take, const std::vector<ProjectMarkerRef>& own_project,
                         bool keep_take_refs, bool keep_project_refs, const std::vector<PlannedMarker>& plan,
                         MarkerMode mode, const std::string& self_guid);
// A copy's Cancel snapshot taking marker edits (so Cancel does not undo them): `edits` applied to
// its events (ApplyMarkerEdits, `detections` for the values), its committed marker refs as `rec`
// holds them now, and `new_sig` (non-empty) as the commit's signature when the snapshot has one.
ItemRules FollowerSnapshot(const ItemRules& snap, const std::vector<MarkerEdit>& edits,
                           const std::vector<Event>* detections, const ItemRules& rec, const std::string& new_sig);

// ---- Preset loaded --------------------------------------------------------------------------------

// Loading a preset clears the item's corrections: the entries of known kinds (detections as
// applied, user events, suppressions) and the applied state. The owned-marker lists (so the
// next Apply still removes RAV's old markers) and unknown-kind entries stay.
void ClearCorrections(ItemRules& rules);

}  // namespace rav
