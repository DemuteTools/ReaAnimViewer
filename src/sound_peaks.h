// SPDX-License-Identifier: MIT
//
// Sound peaks (13-7b, REAPER): a file's mono peaks for waveforms and silence detection.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-7b
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// 13-7b: a file's peaks are read once for the whole file at kPeakRate peaks/s (max |sample| over
// its channels, plus one series per lane L / R for a stereo file: PeakData), incrementally (a bounded slice of work per call, one file at a time) and kept
// in a cache of the last 32 files. Seams for 13-7c: RequestPeaks / ReadPeaks / ReadSoundMedia.

#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

#include "reaper_api.h"
#include "sound_entries.h"
#include "sound_model.h"

namespace rav {

void InitSoundPeaks(void* (*get_func)(const char* name));
// Peaks of `path` over [start, start + length) source seconds at `rate` peaks/s. False when unread.
bool ReadPeaks(const std::string& path, double start, double length, double rate, PeakData* out);

// ---- 13-7b additions ----------------------------------------------------------------------------

constexpr double kPeakRate = 1000.0;

enum class PeakState { Ready, Building, Failed };
// The whole file's peaks (from 0 s at kPeakRate). Building: call again on a later frame (each
// call does at most ~8 ms of reading); `out` is filled only when Ready.
PeakState RequestPeaks(const std::string& path, PeakData* out);
// The file's length in seconds (0 when it does not open). Cached.
double SourceFileLength(const std::string& path);

// What the panel reads of a Sound's media.
struct SoundMedia {
    bool                     ok = false;        // the source was found
    std::string              path;              // the active take's file (Item) / the file (File)
    double                   source_len = 0.0;  // its length (s)
    std::vector<std::string> take_paths;        // Item: one per take
    std::vector<double>      take_lens;
    int                      active_take = 0;
    EntryFacts               facts;             // what detection reads
    double                   item_pos = 0.0;    // Item: its position in the project
};
// Item: the item of that GUID in the current project (take markers, then the chunk's slices);
// File: asset.last_path. False (ok = false) when the source is not found.
bool ReadSoundMedia(const SoundSource& source, SoundMedia* out);
// The file a variation plays: Takes mode = that take's file, else the active take's.
std::string VariationPath(const SoundMedia& media, const SoundData& data, int variation);
// The source's file ("" when not found).
std::string SoundSourcePath(const SoundSource& source);
// The facts of an item (as ReadSoundMedia's Item branch); false for a null item.
bool ReadItemMedia(MediaItem* item, SoundMedia* out);
// Locate...: one sound file chosen in REAPER's file dialog (false when cancelled or unavailable).
bool ChooseSoundFile(const std::string& title, std::string* path);

}  // namespace rav

#endif  // _WIN32
