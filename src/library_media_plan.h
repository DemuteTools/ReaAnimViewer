// SPDX-License-Identifier: MIT
//
// Media Explorer uses (13-7c): what a file or a variation picked in column 4 becomes in a
// library, and the labels / formats the panel shows. Pure, no REAPER, host-tested
// (tests/library_media_plan_test.cpp). The UI (library_media_ui.cpp) wraps UseMediaAt in one
// ModifySoundStore undo point.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "library_tree_edit.h"
#include "media_db.h"
#include "sound_model.h"

namespace rav {

// One file, or one variation of a file, picked in the Media Explorer.
struct MediaPick {
    std::string             path;
    int64_t                 size = -1;
    double                  length = -1.0;  // seconds, -1 = unknown
    int64_t                 mtime = 0;
    bool                    slice = false;  // one variation (`frame`), else the whole file
    SoundFrame              frame;          // slice: the variation
    int                     index = 0;      // slice: its 0-based number (named "stem N", N = index + 1)
    std::vector<SoundFrame> frames;         // whole: the detected variations ({} = none / not ready)
    bool                    frames_known = false;  // whole: `frames` is the file's read result (no read needed)
};

// "C:\a\b.wav" -> "b.wav"; "b.wav" -> "b".
std::string MediaFileName(const std::string& path);
std::string MediaFileStem(const std::string& path);

// Decision 12: the Sound a pick makes (File source, identity from the pick).
NewNodeSpec SoundSpecFor(const MediaPick& pick);

// Decision 13: the footer label for a use at `row` (null = the library root): "Add to Random",
// "Use for Wood", + " (N files)" / " (N variations)" / " (1 variation)". False (label "") when
// the use is refused there (no library, a Switch).
bool MediaUseLabel(const SoundStore& store, const std::string& lib_uid, const LibraryRow* row, int n_picks,
                   bool variations, std::string* label);
// True when a use at `row` adds inside a Random / Blend (undo "RAV: Add sounds"), false when it
// sets a slot ("RAV: Set sound from Media Explorer").
bool MediaUseAddsInside(const SoundStore& store, const std::string& lib_uid, const LibraryRow* row);

// Decision 13: adds the picks' Sounds at `row` (null / empty key = the library root), one
// transaction: false and the store unchanged when refused (`why` says why). `created` gets the
// new Sounds' uids, `toast` the message.
// 13-69 `as_random` (Alt held at the drop, "Make a Random of the selection"): 2+ picks always become
// ONE new Random holding their Sounds, named after the files' common stem: inside a Random / Blend
// as one new child, onto a Sound as a child of the Random that wraps it, on a value / empty slot the
// Random the use makes anyway. `new_random` gets its uid ("" when no Random was made). One pick: the
// plain use.
bool UseMediaAt(SoundStore& store, const std::string& lib_uid, const LibraryRow* row, const std::vector<MediaPick>& picks,
                std::vector<std::string>* created, std::string* toast, std::string* why, bool as_random = false,
                std::string* new_random = nullptr);

// 13-69: the name of a Random made of these files: their stems' longest common start (ignoring
// case, the first file's spelling), without trailing spaces, '_', '-', '.', digits or "(";
// "" when shorter than 2 characters (the type's own name is used).
std::string MediaCommonStem(const std::vector<MediaPick>& picks);

// 13-69: what a drop of `n_picks` files / variations at `row` makes (the drag tooltip), e.g.
// "12 files -> added to the Random Wood", "12 files -> new Random in the Random Wood",
// "2 files -> Step becomes a Random: the old sound + 2 new"; "" when refused there.
std::string MediaDropHint(const SoundStore& store, const std::string& lib_uid, const LibraryRow* row, int n_picks,
                          bool variations, bool as_random);

// 13-69: the results rows a Shift+click selects, from the anchor row to the clicked one (both
// included, in list order); with more than `cap` rows, the `cap` nearest the clicked row. An anchor
// outside [0, n) = the clicked row alone.
std::vector<size_t> MediaRangeRows(int anchor, int clicked, size_t n, size_t cap);
// "12 selected" ("" under 2).
std::string MediaSelectedLabel(size_t n);

// "2ch · 24b · 96k" (unknown parts omitted, "" when none known).
std::string FormatMediaFormat(const DbFile& f);
// "0:02.5", "1:05.0", "" when unknown (< 0).
std::string FormatMediaLength(double seconds);
// The index of the frame holding source time t, -1 when none.
int FrameAt(const std::vector<SoundFrame>& frames, double t);

// An empty search: every file of `db_filter` ("" = all databases), in index order, pointers into
// `files` (no copy; one pass, no sort, cheap for 400k entries).
std::vector<const DbFile*> MediaListAll(const std::vector<DbFile>& files, const std::string& db_filter);

// ---- Whether the listed files are on this PC (decision 8) -----------------------------------------

// The rows come straight from the index: a row is drawn at once, as found, until its file is known
// missing. Each path is checked once for the session, only when its row is on screen, in batches of
// the rows visible in one frame, one batch at a time off the UI thread: rows scrolled past before
// their turn are never checked, and nothing waits behind the checks.
class MediaExistsCache {
public:
    enum class State { Unknown, Checking, Found, Missing };

    State Get(const std::string& path) const;
    bool  Missing(const std::string& path) const { return Get(path) == State::Missing; }
    // A row on screen this frame (an unknown path joins this frame's batch, each once).
    void  Want(const std::string& path);
    // End of a frame: the next batch to check (up to `max` paths, now Checking, `*gen` names it), or
    // {} while a batch is out or nothing is wanted. This frame's wants are dropped either way.
    std::vector<std::string> EndFrame(size_t max, uint64_t* gen);
    // A batch's answers (`found[i]` for `paths[i]`); a batch of an older generation (cleared since)
    // is ignored. Ends the batch out.
    void  Answer(uint64_t gen, const std::vector<std::string>& paths, const std::vector<char>& found);
    // Known some other way (the preview or a use reached the file, or not).
    void  Set(const std::string& path, bool found);
    // Reload: every path is checked again when shown; the batch out is ignored.
    void  Clear();
    bool  BatchOut() const { return out_; }
    size_t Known() const { return state_.size(); }

private:
    std::unordered_map<std::string, State> state_;  // Checking / Found / Missing
    std::vector<std::string>               want_;
    uint64_t                               gen_ = 0;
    bool                                   out_ = false;
};

// ---- Several files used at once: each whole file's variations are read first ---------------------

enum class PickRead { Ready, Reading, Failed, Missing };

// A use waiting for its files' peaks: every whole pick whose variations are not known yet is read
// (once per file, on the panel's worker), then all the Sounds are made together, in one undo point.
struct MediaBatch {
    std::vector<MediaPick> picks;
    std::vector<PickRead>  read;  // one per pick
};

// Slices and whole picks with frames_known are Ready, the other whole picks Reading.
MediaBatch StartMediaBatch(std::vector<MediaPick> picks);
// The files still to read (each once, in pick order).
std::vector<std::string> MediaBatchPathsToRead(const MediaBatch& b);
// A file's result: its variations (ok) or a failed read (its picks become Whole sounds). `length`
// > 0 replaces the picks' length. Every pick of that file still reading is updated.
void MediaBatchSetRead(MediaBatch& b, const std::string& path, bool ok, const std::vector<SoundFrame>& frames,
                       double length);
// A file found missing before it was read: its picks are dropped from the use (named in the toast).
void MediaBatchSetMissing(MediaBatch& b, const std::string& path);
// The picks the use makes Sounds from (every pick but the missing ones).
std::vector<MediaPick> MediaBatchUsedPicks(const MediaBatch& b);
// The names of the missing files (each once).
std::vector<std::string> MediaBatchMissingNames(const MediaBatch& b);
// The number of files still reading (0 = ready to use).
int MediaBatchLeft(const MediaBatch& b);
// The names of the files that could not be read (each once).
std::vector<std::string> MediaBatchFailedNames(const MediaBatch& b);
// "Reading 3 files..." / "Reading 1 file...".
std::string MediaReadingLabel(int n_files);
// Appended to the use's toast: "" when none failed, else "; could not read a.wav, b.wav: added whole".
std::string MediaFailedToastSuffix(const std::vector<std::string>& names);
// Appended to the use's toast: "" when none, else "; missing a.wav, b.wav: not added".
std::string MediaMissingToastSuffix(const std::vector<std::string>& names);

}  // namespace rav
