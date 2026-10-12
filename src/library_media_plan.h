// SPDX-License-Identifier: MIT
//
// Media Explorer uses (13-7c): what a file or a variation picked in column 4 becomes in a
// library, and the labels / formats the panel shows. Pure, no REAPER, host-tested
// (tests/library_media_plan_test.cpp). The UI (library_media_ui.cpp) wraps UseMediaAt in one
// ModifySoundStore undo point.

#pragma once

#include <cstdint>
#include <string>
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
bool UseMediaAt(SoundStore& store, const std::string& lib_uid, const LibraryRow* row, const std::vector<MediaPick>& picks,
                std::vector<std::string>* created, std::string* toast, std::string* why);

// "2ch · 24b · 96k" (unknown parts omitted, "" when none known).
std::string FormatMediaFormat(const DbFile& f);
// "0:02.5", "1:05.0", "" when unknown (< 0).
std::string FormatMediaLength(double seconds);
// The index of the frame holding source time t, -1 when none.
int FrameAt(const std::vector<SoundFrame>& frames, double t);

}  // namespace rav
