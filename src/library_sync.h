// SPDX-License-Identifier: MIT
//
// Library <-> file sync engine (13-77b, pure: no REAPER, no _WIN32). A linked library's project copy ("mine"), its
// .ravlib ("file") and the common version stored by 13-77a ("base") are three .ravlib texts of ONE library, all in the
// project's uid space (TranslateUidsText turns the file's into it and back). The diff works on the lines of those
// texts, so a field a later story adds shows up as "<key> changed" with no code here.
//
// A CHANGE is one fact (a field, a Sound / variation / container added or removed, a Switch case, a Switch Group value,
// a rename ...) keyed so it survives a rebuild. Its side: Here (mine moved from the base, the file did not), File, Both
// (both moved and they differ; also every difference when there is no common version). Equal on both sides is never a
// change. Apply takes a direction per change (Up = Send mine, Down = Get file's): the chosen side's value goes into the
// project copy, the file and the new base, so a change dropped (here Down, file Up) reverts that side. Nothing else
// moves: the rest of the file stays as it is.
//
// 13-77c adds the change-by-change window on top of this: it only builds the `picks` map.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sound_model.h"

namespace rav {

enum class SyncSide { Here, File, Both };
enum class SyncDir { Up, Down };  // Up = Send mine (mine's value wins), Down = Get file's

struct SyncChange {
    std::string key;           // stable: kind|node|...
    SyncSide    side = SyncSide::Both;
    std::string node;          // the container row it sits under, or "grp:<group uid>"
    std::string subject;       // the node it is about (a Sound's own changes sit under its parent, `node`)
    std::string text;          // one line: "Gain of FS_x -3 -> -8 dB"
    std::string here_text;     // the value mine holds ("-3 dB"; existence: "present" / "absent")
    std::string file_text;     // the value the file holds
    std::string label;         // "Gain", "Name", "Variation 3" ...
    bool        adds_container = false;  // an Exists that adds a container (a row the project does not have yet)
    int         depth = 0;     // depth of the node it concerns (additions go parents first, removals children first)
    std::vector<std::string> needs;  // keys of same-side changes it needs
};

struct SyncDiff {
    std::vector<SyncChange> changes;
    int  here = 0, file = 0, both = 0;  // counts by side
    bool had_base = false;
    bool file_untouched = false;        // the file is as it was when this project last read / wrote it
    std::vector<std::string> folds;     // keys equal on both sides but not in the base: the base takes them at Apply
    std::map<std::string, std::string> parent;  // every container / Sound uid of the three texts -> its parent uid
    std::set<std::string> mine_nodes;   // the node uids the project copy has
    std::string root;                   // the library's root node uid
    const SyncChange* Find(const std::string& key) const;
};

// Counts of the changes sitting under one row.
struct LibSyncCounts {
    int here = 0, file = 0, both = 0;
    int Total() const { return here + file + both; }
};

// base_text: "" = no common version (every difference is a Both change). `file_untouched`: the file's header says
// nobody wrote it since the base was taken (by / at equal): whatever differs between the base and the file is then a
// project-local adaptation of the merge (a renamed library, the project's roles / default), never a file change.
SyncDiff DiffLibrary(const std::string& base_text, const std::string& mine_text, const std::string& file_text,
                     bool file_untouched = false);

// The keys `key` needs, transitively (not itself); the keys that need `key`, transitively.
std::vector<std::string> NeedsClosure(const SyncDiff& d, const std::string& key);
std::vector<std::string> DependentsClosure(const SyncDiff& d, const std::string& key);

// Closes a selection: adopting a change (Here + Up, File + Down) also takes everything it needs, dropping one (Here +
// Down, File + Up) also takes everything that needs it, in the same direction. Both changes pull nothing. A pull wins
// over an earlier pick of the same key (picks are walked in key order).
std::map<std::string, SyncDir> CloseSelection(const SyncDiff& d, const std::map<std::string, SyncDir>& picks);

// A container's gesture (decision 7): the keys of its own open changes, plus the ones under containers added below it
// that the project does not have yet (a Random new in the file has no row to click). The caller closes the result.
std::vector<std::string> ContainerChangeKeys(const SyncDiff& d, const std::string& container);
// Counts of the changes sitting under `node` (own only).
LibSyncCounts CountsOf(const SyncDiff& d, const std::string& node);
LibSyncCounts TotalCounts(const SyncDiff& d);

struct SyncResult {
    std::string mine, file, base;  // new .ravlib texts (project uid space); base "" = still no common version
    int         sent = 0;          // changes applied Up
    int         taken = 0;         // changes applied Down
    bool        mine_changed = false, file_changed = false;
};
// Applies the picked changes (already closed). False (result untouched) when a text is not a ravlib. Pure: the caller
// re-reads the file, writes it, then the store.
bool ApplySync(const std::string& base_text, const std::string& mine_text, const std::string& file_text,
               const SyncDiff& diff, const std::map<std::string, SyncDir>& picks, SyncResult* out);

// Puts a merged project copy (the `mine` text of an Apply, parsed: one library and its groups) into the store: the
// library's nodes replaced, its project-local fields kept (ravlib_path, kit_offered, File sources' item_guid by node
// uid), the groups merged additively (a group / value is never removed: they are shared by every library). False when
// the library is not in the store.
bool ReplaceLibraryFromMerge(SoundStore& store, const std::string& lib_uid, const SoundStore& merged);

// file uid <-> project uid, over a whole .ravlib text (header and every value of every line).
std::string TranslateUidsText(const std::string& text, const std::vector<std::pair<std::string, std::string>>& uid_map,
                              bool to_project);

// ---- the gates around the I/O (pure decisions the UI layer calls) ------------------------------------------
// FNV-1a 64 of a text: the base's hash, the file's hash.
uint64_t SyncTextHash(const std::string& text);
// What the file looked like when the list was built / what it is now (size -1 = gone).
struct SyncFileStamp {
    int64_t  size = -1;
    int64_t  mtime = 0;
    uint64_t hash = 0;  // of the text; 0 when not read
    bool     operator==(const SyncFileStamp& o) const { return size == o.size && mtime == o.mtime && (hash == 0 || o.hash == 0 || hash == o.hash); }
};
enum class ApplyGate { Go, FileChanged, FileGone };
// Apply re-reads the file first: if it moved since the list, the list is rebuilt and nothing is written.
ApplyGate DecideApplyGate(const SyncFileStamp& listed, const SyncFileStamp& now);
// Ctrl+Z / redo of an Apply (decision 10, session only). The timer compares the project's base now with the journal.
struct SyncJournal {
    std::string lib_uid, file;       // file name
    std::string before, after;       // the file text before / after the Apply (file uid space, as written)
    uint64_t    base_before = 0, base_after = 0;  // hashes of the library's common version text
};
enum class JournalStep { None, RestoreBefore, RestoreAfter, FileWrittenSince };
// `base_now`: hash of the library's common version now; `file_hash`: hash of the file's text, `file_exists` false when
// gone. Undo: the base is back at `before` and the file still is `after` -> RestoreBefore (the file was written since:
// FileWrittenSince, only the project is back). Redo: the base is at `after` again and the file is still `before`.
JournalStep DecideJournal(const SyncJournal& j, uint64_t base_now, bool file_exists, uint64_t file_hash);

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
// "Applied: N written to <file> (the version before is kept as <file>.bak), M taken here".
std::string SyncAppliedToast(int sent, int taken, const std::string& file);

}  // namespace rav
