// SPDX-License-Identifier: MIT
//
// Library <-> file sync engine (13-77b, pure: no REAPER, no _WIN32). A linked library's project copy ("mine"), its
// .ravlib ("file") and TWO common versions kept by the project (review 13-77b H3), all four .ravlib texts of ONE library
// in the project's uid space (TranslateUidsText turns the file's into it and back):
//   - base:      the merged project copy at the last sync (13-77a: what the merge adapted is already in it);
//   - file base: the file's exact text as this project last read or wrote it (translated into project uids).
// Per key: changed here = mine != base; changed in the file = file != file base; both = both moved and differ. A key
// where base and file base differ while neither side moved is a project-local adaptation of the merge (a library renamed
// on a clash, the project's group name or default, the roles 13-23 filtered): never listed, never written, never taken.
// No clock is involved: a hand edit of the file is seen exactly. The diff works on the lines of those texts, so a field a
// later story adds shows up as "<key> changed" with no code here.
//
// A CHANGE is one fact (a field, a Sound / variation / container added, removed or moved, a Switch case, a Switch Group
// value, a rename ...) keyed so it survives a rebuild. Equal on both sides is never a change. Apply takes a direction per
// change (Up = Send mine, Down = Get file's): the chosen side's value goes into the project copy, the file and the base,
// so a change dropped (here Down, file Up) reverts that side. Nothing else moves: the rest of the file stays as it is.
//
// 13-77c adds the change-by-change window on top of this: it only builds the `picks` map. CloseSelection walks the picks
// in key order and a pull wins over an earlier pick of the same key: with mixed directions on changes that depend on each
// other, the last one in key order decides (review 13-77b L2). 13-77c's selection model resolves the directions before it
// calls Apply, so the engine never sees such a mix from it.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sound_model.h"

namespace rav {

enum class SyncSide { Here, File, Both };
enum class SyncDir { Up, Down };  // Up = Send mine (mine's value wins), Down = Get file's

struct SyncChange {
    std::string key;           // stable: kind|node|...
    SyncSide    side = SyncSide::Both;
    std::string node;          // the container it sits under, or "grp:<group uid>" (a move: its new parent)
    std::string subject;       // the node it is about (a Sound's own changes sit under its parent, `node`)
    std::string row;           // the project's row whose list and gesture hold it (review 13-77b H5): `node` when the
                               // project has it, else its nearest ancestor the project has; the root for "grp:" ones
    std::string text;          // one line: "Gain of FS_x -3 -> -8 dB"
    std::string here_text;     // the value mine holds ("-3 dB"; existence: "present" / "absent")
    std::string file_text;     // the value the file holds
    std::string label;         // "Gain", "Name", "Variation 3" ...
    bool        adds_container = false;  // an Exists that adds a container (a row the project does not have yet)
    int         depth = 0;     // depth of the node it concerns (additions go parents first, removals children first)
    std::vector<std::string> needs;  // keys of same-side changes it needs
};

// Counts of the changes sitting under one row.
struct LibSyncCounts {
    int here = 0, file = 0, both = 0;
    int Total() const { return here + file + both; }
};

struct SyncDiff {
    std::vector<SyncChange> changes;
    int  here = 0, file = 0, both = 0;  // counts by side
    bool had_base = false;              // both common versions known (else every difference is a Both change)
    bool file_untouched = false;        // the file holds what this project last read / wrote (no key moved in it)
    std::vector<std::string> folds;     // keys equal on both sides but not in the base: the base takes them at Apply
    std::vector<std::string> file_folds;  // keys equal on both sides but not in the file base: the file base takes them
    std::map<std::string, std::string> parent;  // every container / Sound uid of the texts -> its parent uid (mine first)
    std::set<std::string> mine_nodes;   // the node uids the project copy has
    std::string root;                   // the library's root node uid
    std::map<std::string, LibSyncCounts> row_counts;  // row -> counts of its changes (built once per diff, review M8)
    // Review 13-77c H1: a key -> position index and the reverse "who needs it" lists, built once per diff (DiffLibrary,
    // or BuildIndex() for a diff assembled by hand). Find / DependentsClosure use them while they match `changes` (same
    // size), and fall back to a scan otherwise, so a hand-edited diff stays correct.
    std::unordered_map<std::string, size_t>              index;
    std::unordered_map<std::string, std::vector<size_t>> needed_by;  // key -> positions of the changes whose needs hold it
    void BuildIndex();
    bool IndexValid() const { return !changes.empty() && index.size() == changes.size(); }
    const SyncChange* Find(const std::string& key) const;
};

// base_text / file_base_text: the two common versions ("" = unknown). When either is unknown (no common version, or a
// project saved before review 13-77b kept only the base), every difference is a Both change and nothing is ever
// assumed untouched.
SyncDiff DiffLibrary(const std::string& base_text, const std::string& file_base_text, const std::string& mine_text,
                     const std::string& file_text);
// The same with the file base = the base (no adaptation): the shape of the host tests' fixtures.
SyncDiff DiffLibrary(const std::string& base_text, const std::string& mine_text, const std::string& file_text);

// The keys `key` needs, transitively (not itself); the keys that need `key`, transitively.
std::vector<std::string> NeedsClosure(const SyncDiff& d, const std::string& key);
std::vector<std::string> DependentsClosure(const SyncDiff& d, const std::string& key);
// An ADDED element (a Sound / container / value / variation / case one side has and the other lacks) is an addition by
// the side that added it (Here: the project, File: the global); false when the change is not one.
bool SyncIsAddition(const SyncChange& c, SyncSide* adder);
// Is there a box on that side (Up = project, Down = global)? False only for the missing side of an addition.
bool SyncSideAvailable(const SyncChange& c, SyncDir dir);
// DependentsClosure for a drop in direction `dir`: never takes an addition of the side being dropped, nor what is
// reached only through it (the rule: an addition is never deleted by the other side).
std::vector<std::string> DropDependents(const SyncDiff& d, const std::string& key, SyncDir dir);

// Closes a selection: adopting a change (Here + Up, File + Down) also takes everything it needs, dropping one (Here +
// Down, File + Up) also takes everything that needs it, in the same direction. Both changes pull nothing. A pull wins
// over an earlier pick of the same key (picks are walked in key order).
std::map<std::string, SyncDir> CloseSelection(const SyncDiff& d, const std::map<std::string, SyncDir>& picks);

// A container's gesture (decision 7): the keys whose `row` is that container: its own open changes, the ones under
// containers added below it that the project does not have yet (a Random new in the file has no row to click), and, for
// the library's root, the Switch Group ones. The caller closes the result.
std::vector<std::string> ContainerChangeKeys(const SyncDiff& d, const std::string& container);
// Counts of the changes whose row is `node` (the same rule as ContainerChangeKeys: tag, bar and gesture agree).
LibSyncCounts CountsOf(const SyncDiff& d, const std::string& node);
LibSyncCounts TotalCounts(const SyncDiff& d);

struct SyncResult {
    std::string mine, file;        // new .ravlib texts (project uid space)
    std::string base, file_base;   // the new common versions; "" = still none (a partial Apply with no common version).
                                   // Built key by key (second review H1): each takes the chosen value of every applied
                                   // change and every key equal on both sides; an unpicked change keeps its old value
                                   // in both, so it is still listed, on the same side, after a partial Apply.
    int         sent = 0;          // changes applied Up (only those that really changed the file / the base)
    int         taken = 0;         // changes applied Down
    bool        mine_changed = false, file_changed = false;
    std::vector<std::string> open; // picked changes that could not be applied (a move whose new parent is not there,
                                   // a move that would make a loop): they stay listed, nothing of them moved
};
// Applies the picked changes (already closed). False (result untouched) when a text is not a ravlib. Pure: the caller
// re-reads the file, writes it, then the store. A node the project copy (or the file) already had is never dropped: a
// move whose new parent is missing on that side, or that would make a loop, keeps the old parent and stays open.
bool ApplySync(const std::string& base_text, const std::string& file_base_text, const std::string& mine_text,
               const std::string& file_text, const SyncDiff& diff, const std::map<std::string, SyncDir>& picks,
               SyncResult* out);
bool ApplySync(const std::string& base_text, const std::string& mine_text, const std::string& file_text,
               const SyncDiff& diff, const std::map<std::string, SyncDir>& picks, SyncResult* out);

// The changes from `old_text` to `new_text`, carried onto `onto` (a three-way rebase: on a key all three differ,
// new_text's value wins). False when a text is not a ravlib. Second review: the file base follows the text really
// written (H1), the base absorbs what the store merge adapted (H2), and the edits made during an Apply's round trip go
// onto its result (M2).
bool CarryChanges(const std::string& old_text, const std::string& new_text, const std::string& onto, std::string* out);

// Second review H3: where an answer of the worker (an Apply, a Ctrl+Z journal) goes. It acts only on the project shown
// now (the store calls work on the shown project); another open tab's answer waits until that tab is shown again.
enum class SyncRoute { Drop, Handle, Hold };
SyncRoute RouteSyncAnswer(bool tab_open, bool tab_shown);

// Puts a merged project copy (the `mine` text of an Apply, parsed: one library and its groups) into the store: the
// library's nodes replaced, its project-local fields kept (ravlib_path, kit_offered, File sources' item_guid by node
// uid), the groups merged additively (a group / value is never removed: they are shared by every library). A group the
// library did not use before (`before`, the project copy's text before the Apply, parsed; null = unknown) keeps its
// project name, default and values: only the values it lacks are added. A value added to a group of the project keeps
// only the roles no other value of that group holds (13-23: the project's role map stands). A library name taken by
// another library of the project is made unique. False when the library is not in the store.
bool ReplaceLibraryFromMerge(SoundStore& store, const std::string& lib_uid, const SoundStore& merged,
                             const SoundStore* before = nullptr);

// file uid <-> project uid, over a whole .ravlib text (header and every value of every line).
std::string TranslateUidsText(const std::string& text, const std::vector<std::pair<std::string, std::string>>& uid_map,
                              bool to_project);
// 13-11 decision 7 / spec decision 5 (review 13-77b M4): the uids of the file's groups and values that the project
// knows under another uid, matched by name (a group by its name; a value by its name inside its group), never matching
// a project uid the file already uses or another file uid already maps to. `file_proj_text` is the file translated
// with `uid_map`; the result adds to `uid_map` (file uid -> project uid). An Apply records it in the link's map.
std::vector<std::pair<std::string, std::string>> NameMergeMap(const std::vector<SwitchGroup>& project_groups,
                                                              const std::string& file_proj_text,
                                                              const std::vector<std::pair<std::string, std::string>>& uid_map);

// ---- the gates around the I/O (pure decisions the UI layer calls) ------------------------------------------
// FNV-1a 64 of a text: the base's hash, the file's hash.
uint64_t SyncTextHash(const std::string& text);
// A fingerprint of what a diff lists: its root and, per change, its key, side and the three texts (review 13-77c M3 / L3).
// The window ticks against one signature; an Apply carrying it stops when the list it meets is another.
uint64_t SyncDiffSignature(const SyncDiff& d);
// What the file looked like when the list was built / what it is now (size -1 = gone).
struct SyncFileStamp {
    int64_t  size = -1;
    int64_t  mtime = 0;
    uint64_t hash = 0;  // of the text; 0 when not read
    bool     operator==(const SyncFileStamp& o) const { return size == o.size && mtime == o.mtime && (hash == 0 || o.hash == 0 || hash == o.hash); }
};
enum class ApplyGate { Go, FileChanged, FileGone };
// Apply re-reads the file first: if it moved since the list, the list is rebuilt and nothing is written. The same gate
// runs again on a stat just before the written file replaces the old one (review 13-77b M6).
ApplyGate DecideApplyGate(const SyncFileStamp& listed, const SyncFileStamp& now);

// The link's state the Ctrl+Z journal follows: the two common versions and the sync count (every Apply moves it, so an
// Apply that changed neither text still has an undo point of its own).
uint64_t LinkStateHash(const LinkBase& b);
// What the link stores after an Apply (decision: the result's common versions when it has them, else the old ones; the
// uid map grown by the name merge; one more sync; synced now; the file's header by / at kept for display only).
LinkBase NextLinkBase(const LinkBase* old, const std::string& lib_uid, const SyncResult& r,
                      const std::vector<std::pair<std::string, std::string>>& uid_map, int64_t now,
                      const std::string& file_by, int64_t file_at);

// Ctrl+Z / redo of an Apply (decision 10, session only). One journal per Apply that wrote the file.
struct SyncJournal {
    std::string lib_uid, file;       // file name
    std::string before, after;       // the file text before / after the Apply (file uid space, as written)
    uint64_t    base_before = 0, base_after = 0;  // LinkStateHash of the library's link before / after
};
enum class JournalStep { None, RestoreBefore, RestoreAfter, FileWrittenSince };
// One journal alone (kept for the chain below and its tests). `base_now`: the link's state hash now; `file_hash`: hash
// of the file's text, `file_exists` false when gone.
JournalStep DecideJournal(const SyncJournal& j, uint64_t base_now, bool file_exists, uint64_t file_hash);
// The journals of ONE library, oldest first, with their undone flags (review 13-77b H2 + M7). Acts only on a real undo /
// redo of an Apply: `undo_names_sync` = REAPER's next redo is "RAV: Sync with file" (an undo just happened), and
// `redo_names_sync` = its next undo is (a redo just happened). Several quick Ctrl+Z chain: the file goes back to the
// oldest undone Apply's `before` when it still holds the newest applied one's `after` (and every Apply in between
// follows the previous one); the same forward on redo.
struct JournalPlan {
    JournalStep       step = JournalStep::None;
    std::string       write;           // the text to write (RestoreBefore / RestoreAfter)
    std::vector<bool> undone;          // the flags after the step
    bool              drop = false;    // FileWrittenSince: these journals are of no more use
};
JournalPlan DecideJournalChain(const std::vector<SyncJournal>& js, const std::vector<bool>& undone, uint64_t state_now,
                               bool undo_names_sync, bool redo_names_sync, bool file_exists, uint64_t file_hash);
// Before the file is read (UI thread): would DecideJournalChain act at all with this state and these gates?
bool JournalChainCandidate(const std::vector<SyncJournal>& js, const std::vector<bool>& undone, uint64_t state_now,
                           bool undo_names_sync, bool redo_names_sync);

// ---- texts (mockup v3 wording) -------------------------------------------------------------------------
// The tree tag of a container: "edited" (here only), "in file" (file only), "both" (the rest), " · N" when N > 1.
// Empty when it has no change.
std::string SyncTagText(const LibSyncCounts& c);
// Its tooltip: "Changed here, not in the file yet: N changes. Click to see what" (and its two siblings).
std::string SyncTagTip(const LibSyncCounts& c);
// The library row's mark texts: ↑N = here + both, ↓N = file + both ("" when 0) and the tooltip lines.
std::string SyncRowUpText(const LibSyncCounts& c);
std::string SyncRowDownText(const LibSyncCounts& c);
std::string SyncRowTip(const LibSyncCounts& c);
// "N changes made here, not in the file" ... one line per non-empty count.
std::string SyncCountLine(int n, SyncSide side);
// "Applied: N written to <file> (the version before is kept as <file>.bak), M taken here"; with `open` > 0:
// " · N could not be applied: they stay listed".
std::string SyncAppliedToast(int sent, int taken, const std::string& file, int open = 0);

}  // namespace rav
