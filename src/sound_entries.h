// SPDX-License-Identifier: MIT
//
// Sound entries (13-3): a Sound's variations (frames) from its takes, take-marker slices or silence.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-3
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

// A take marker of the entry's take, in source seconds.
struct TakeMarkerPos {
    double      pos = 0.0;
    std::string name;
};

// What detection reads of an entry (the REAPER side fills it; tests build it by hand).
struct EntryFacts {
    double                  source_len = 0.0;  // the active take's source length (s)
    double                  start_offs = 0.0;  // the active take's start in its source (s)
    double                  item_len = 0.0;    // the item's length (s)
    std::vector<SoundFrame> takes;             // one per take of the item (start = its start offset)
    std::vector<Slice>      slices;            // the active take's slices (take markers or .rpp text)
};

// Slices from an item's .rpp chunk text (the fallback reader of take markers, spike 13-0 A):
// region lines (flag &1) ignored, =START shifts zero, slices of <= 1 ms dropped. A slice of
// length -1 runs to the source end (no =END); DetectFrames resolves it.
std::vector<Slice> SlicesFromRppText(const std::string& item_chunk_text);
// Slices between consecutive take markers (the last one runs to source_len). Needs >= 2 markers.
std::vector<Slice> SlicesFromTakeMarkers(const std::vector<TakeMarkerPos>& markers, double source_len);
// Variations split on silence of the source's peaks.
std::vector<SoundFrame> SplitOnSilence(const PeakData& peaks, const SilenceParams& params, double source_len);
// The detected (pre-edit) frames of an entry for a mode. `peaks` may be null (no Silence mode then).
// The Whole fallback is the item's stretch inside [0, source_len]; {} when that is 1 ms or less.
std::vector<SoundFrame> DetectFrames(const EntryFacts& facts, DetectMode mode, const SilenceParams& silence,
                                     const PeakData* peaks);
// Keeps the frames inside [0, source_len], drops empty ones, sorts by start.
void ClampFrames(std::vector<SoundFrame>& frames, double source_len);
// Back to the detected frames (hand edits dropped, `edited` cleared).
void ResetToDetected(SoundData& data, const std::vector<SoundFrame>& detected);
// The frame's peak (dBFS), -inf when the peaks do not cover it.
double FramePeakDb(const PeakData& peaks, const SoundFrame& frame);
// Sets each frame's db so its peak reaches target_db: every frame (whole) or the `selection` indices.
// Sets `hand` on each frame it changes; the caller (the Normalize gesture) sets `data.edited`.
void NormalizeFrames(std::vector<SoundFrame>& frames, const PeakData& peaks, double target_db, bool whole,
                     const std::vector<int>& selection);

// The default mode of a new entry (DECISIONS Q24): Takes when >= 2 takes, else TakeMarkers when
// >= 2 slices, else Silence when peaks exist, else Whole.
DetectMode DefaultDetectMode(const EntryFacts& facts, bool has_peaks);

// Hand edits (pre-edit). Each sets `hand` on the touched frame and `data.edited`; frames never
// overlap (a frame is clamped between its neighbours' edges).
// Moves / resizes frame i inside [previous end, next start] and [0, source_len]; unchanged when
// that leaves 1 ms or less.
void TrimFrame(SoundData& data, int i, double start, double length, double source_len);
// Adds a frame clamped into the gap that holds `start`; its index, -1 when `start` is inside a
// frame or there is no room.
int AddFrame(SoundData& data, double start, double length, double source_len);
// Splits frame i at source time `at` into 2 frames (on and db kept); false when `at` is outside it.
bool SplitFrame(SoundData& data, int i, double at);
void SetFrameOn(SoundData& data, int i, bool on);
void SetFrameDb(SoundData& data, int i, double db);

}  // namespace rav
