// SPDX-License-Identifier: MIT
//
// 13-77c: what the "Sync with file" window ticks (pure: no REAPER, no _WIN32). The mockup v3's own algorithm
// (put / pull / prune / toggleChange / chooseDir / groupToggle / groupDir) over the changes of a SyncDiff
// (library_sync.h). Per change a tick {on, dir, auto, by, why}. Adopting a change (Here + Up, File + Down) ticks what it
// needs, dropping one (Here + Down, File + Up) ticks what depends on it, in the same direction, with `by` = the change
// that asked and `why` = needs / goes; ticking never refuses. A `both` change pulls nothing and is ticked only once a
// side is chosen. Unticking a change prunes what depended on it (a fixpoint). The window only draws this and calls
// RequestSyncApply with Picks(), whose directions are already resolved (the engine's "last pick wins" never decides).
// The selection holds no pointer to the diff: every call takes the current one, and Rekey keeps the ticks across a
// rebuilt diff by change key.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "library_sync.h"

namespace rav {

struct SyncTick {
    bool        on = false;
    bool        has_dir = false;     // a direction was chosen (a `both` change needs one to be ticked)
    SyncDir     dir = SyncDir::Up;
    SyncSide    side = SyncSide::Both;  // the change's side when ticked (Rekey drops a tick whose side moved)
    bool        auto_ = false;       // ticked for you because another change needs it / goes with it
    std::string by;                  // the change key that asked (auto_)
    bool        needs_why = true;    // auto_: true = "needed by", false = "goes with"
    std::string val;                 // the change's text and both values when ticked (Rekey drops a tick whose value moved)
};

// What one gesture did besides the change(s) it was about (for the toasts and the folds).
struct SyncEffect {
    std::vector<std::string> ticked;    // keys ticked for the user
    std::vector<std::string> unticked;  // keys unticked with the one(s) clicked (they went together)
    bool need_side = false;             // ticking a `both` change with no side chosen: refused, stays unticked
    int  skipped_both = 0;              // a parent's tick passed over this many unsided `both` changes
};

enum class SyncTri { Disabled, None, Some, All };

struct SyncCountsTicked {
    int ticked = 0, up = 0, down = 0, auto_ = 0;
};

// ---- the tree of changed branches (ids: container uid, "grp" = Switch Groups, "grp:<group uid>") ----------------
// A node's parent id ("" = none).
std::string SyncParentOf(const SyncDiff& d, const std::string& id);
// Every id holding a change (c.node) and all their ancestors.
std::vector<std::string> SyncChangedNodes(const SyncDiff& d);
// The ids with no parent among `nodes`, the library root first, then "grp".
std::vector<std::string> SyncTopNodes(const SyncDiff& d, const std::vector<std::string>& nodes);
// Keys of the changes at or under `id` ("*" = all, in diff order).
std::vector<std::string> SyncKeysUnder(const SyncDiff& d, const std::string& id);
// Keys of the changes sitting on exactly `id`.
std::vector<std::string> SyncKeysAt(const SyncDiff& d, const std::string& id);
// A node the project does not have (new in the file): its name and type word ("Random") from its own addition / removal
// change; "" when none. Switch Groups: "Switch Groups"; "grp:x": the group's name from its change text.
std::string SyncGhostName(const SyncDiff& d, const std::string& id);
std::string SyncGhostType(const SyncDiff& d, const std::string& id);
// "N changes" / "1 change".
std::string SyncChangesWord(int n);
// The default direction of a change (Here: Up, File: Down); Both: none.
bool SyncDefaultDir(const SyncChange& c, SyncDir* out);
// A parent's / container's "↑ Send mine" ("↓ Get file's"): the keys among `keys` that direction applies to: Up = changes
// made here + both-sides ones, Down = changes made in the file + both-sides ones. A change made on the other side is
// never selected (it stays listed): sending mine must not revert what another project wrote, nor the reverse.
std::vector<std::string> SyncKeysForDir(const SyncDiff& d, const std::vector<std::string>& keys, SyncDir dir);
// "↑ mine N" / "↓ file's N" counts of a set of keys: Up = changes made here + both, Down = file + both.
void SyncDirCounts(const SyncDiff& d, const std::vector<std::string>& keys, int* up, int* down);

// ---- v5 (Fork-style window): one tick per side ------------------------------------------------------------------
// A tick on the project's side (R) = keep mine = the engine's Up; on the global's side (L) = keep global = Down.
// An ADDED element (a Sound / container / value / variation / case one side has and the other lacks) has ONE box, on
// the side that added it ("add it to the other side too"); unticked it stays only where it is. Removals and
// modifications have two boxes; ticking the unchanged side reverts the other side's change.
// The side that added the change (Here: the project, File: the global), false when it is not an addition.
bool SyncIsAddition(const SyncChange& c, SyncSide* adder);
// Is there a box on that side (Up = project / R, Down = global / L)? False only for the missing side of an addition.
bool SyncSideAvailable(const SyncChange& c, SyncDir dir);
// What a ticked change gives each side, for the footer: the side that ends up with `noun` or drops it.
struct SyncFooterSide {
    std::vector<std::string> gets, drops;
};
struct SyncFooter {
    int            changes = 0;
    SyncFooterSide global, project;
};
// A name shown for a change in the footer (its text).
std::string SyncFooterNoun(const SyncChange& c);
// "Apply: N changes · the global gets a, b, c +2 more, drops x · this project gets ..." ("" when nothing ticked); `full`
// (when not null) gets the same with no cut (the tooltip).
std::string SyncFooterSummary(const SyncFooter& f, std::string* full);

// Names: the common start / end of a set of sound names is hidden (the start up to a space or '_', the end from a '_').
struct SyncNameTrim {
    std::string pfx, sfx;
};
SyncNameTrim SyncCommonTrim(const std::vector<std::string>& names);
std::string  SyncTrimName(const SyncNameTrim& t, const std::string& name);
// The differing part of two names, token-wise (split before a space or '_'): [a0, a1) in a, [b0, b1) in b.
void SyncDiffRange(const std::string& a, const std::string& b, size_t* a0, size_t* a1, size_t* b0, size_t* b1);

class SyncSelection {
public:
    // v5: the box of one side of one change. Ticked there: off (+ prune). Else that side kept (the other side's tick
    // goes), with its closure. A side an addition does not have: nothing.
    SyncEffect ToggleSide(const SyncDiff& d, const std::string& key, SyncDir dir);
    // v5: every change that has no tick yet (a rebuilt diff shows new ones) and was made on one side only is ticked on
    // the side that made it (a conflict is never pre-ticked). Call after Rekey.
    void PreTick(const SyncDiff& d);
    // v5: "Keep all mine" (Up) / "Keep all global" (Down) on `keys`: that side kept everywhere, conflicts settled. An
    // addition made on the other side is unticked (it stays only where it is). Closure ticked for you.
    SyncEffect KeepAll(const SyncDiff& d, const std::vector<std::string>& keys, SyncDir dir);
    // v5: the box of Keep all: Disabled when empty, All when every change keeps that side (an addition of the other
    // side: unticked), Some, None.
    SyncTri KeepTri(const SyncDiff& d, const std::vector<std::string>& keys, SyncDir dir) const;
    // v5: the ticked changes of `keys` off (+ prune); an addition of the other side goes back to ticked (its default).
    SyncEffect KeepClear(const SyncDiff& d, const std::vector<std::string>& keys, SyncDir dir);
    // v5: what each side gets from the ticks (the footer).
    SyncFooter Footer(const SyncDiff& d) const;

    bool IsOn(const std::string& key) const;
    const SyncTick* Tick(const std::string& key) const;

    // The checkbox of one change. Ticked: off + prune; off: its chosen / default direction + closure.
    SyncEffect Toggle(const SyncDiff& d, const std::string& key);
    // ↑ Send mine / ↓ Get file's on one change: ticks it in that direction + closure.
    SyncEffect Choose(const SyncDiff& d, const std::string& key, SyncDir dir);
    // A parent's checkbox: all ticked -> all off (+ prune); else tick every unticked one except unsided `both`.
    SyncEffect ToggleGroup(const SyncDiff& d, const std::vector<std::string>& keys);
    // A parent's ↑ mine / ↓ file's: the changes that direction applies to (+ the `both` ones) ticked that way.
    SyncEffect GroupDir(const SyncDiff& d, const std::vector<std::string>& keys, SyncDir dir);

    SyncTri TriState(const SyncDiff& d, const std::vector<std::string>& keys) const;
    SyncCountsTicked Counts(const SyncDiff& d) const;
    // The keys of the other ticked changes (same direction) that need `key` (adopt) / go with it (drop), transitively.
    std::vector<std::string> NeededBy(const SyncDiff& d, const std::string& key) const;
    // NeededBy of every ticked key that has some (computed together, for the window to cache per selection change).
    std::map<std::string, std::vector<std::string>> NeededByAll(const SyncDiff& d) const;
    // Bumps on every gesture / rebuild that may change a tick (the window's cache key).
    uint64_t Version() const { return ver_; }
    // For RequestSyncApply: the ticked changes and their directions (consistent: the engine's closure changes nothing).
    std::map<std::string, SyncDir> Picks(const SyncDiff& d) const;
    // The diff was rebuilt: a tick stays when its change is still there with the same side, the others drop; then prune.
    void Rekey(const SyncDiff& now);
    void Clear() { ticks_.clear(); ++ver_; }

private:
    void Put(const SyncDiff& d, const SyncChange& c, SyncDir dir, bool auto_, const std::string& by, bool needs_why);
    void Pull(const SyncDiff& d, const SyncChange& c, SyncDir dir, SyncEffect* fx);
    std::vector<std::string> Prune(const SyncDiff& d);
    void Off(const SyncDiff& d, const SyncChange& c);
    bool SetDir(const SyncDiff& d, const SyncChange& c, SyncDir dir, SyncEffect* fx);
    bool TickKey(const SyncDiff& d, const SyncChange& c, SyncEffect* fx);

    std::map<std::string, SyncTick> ticks_;
    uint64_t                        ver_ = 0;
};

// ---- the toasts (mockup wording) and the footer -------------------------------------------------------------------
// "" when there is nothing to say. `clicked` = the keys the gesture was about (left out of the lists).
std::string SyncEffectToast(const SyncDiff& d, const SyncEffect& fx);
// "N ticked: ↑ x to the file, ↓ y taken here · z ticked for you because another needs it" / "Nothing ticked. N changes
// waiting." / "Nothing left: this project and the file agree." (`left` = open changes).
std::string SyncFooterText(const SyncCountsTicked& c, int left);

}  // namespace rav
