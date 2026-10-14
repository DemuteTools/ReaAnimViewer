// SPDX-License-Identifier: MIT
//
// Sound peaks (13-7b, REAPER): a file's mono peaks for waveforms and silence detection.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-7b
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// 13-7b: a file's peaks are read once for the whole file (max |sample| over its channels, plus one
// series per lane L / R for a stereo file: PeakData), one file at a time, and kept in a cache of
// the last 32 files. Seams for 13-7c: RequestPeaks / ReadPeaks / ReadSoundMedia.
// 13-55: one reader for the Sound panel, Edit and the Media Explorer, on its own thread, reading
// REAPER's .reapeaks peak file (built and written when missing); see sound_peaks.cpp.

#pragma once

#ifdef _WIN32

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "file_change_check.h"
#include "reaper_api.h"
#include "sound_entries.h"
#include "sound_model.h"
#include "wave_tiles_plan.h"

namespace rav {

void InitSoundPeaks(void* (*get_func)(const char* name));
// Peaks of `path` over [start, start + length) source seconds at `rate` peaks/s. False when unread.
bool ReadPeaks(const std::string& path, double start, double length, double rate, PeakData* out);

// ---- 13-7b additions ----------------------------------------------------------------------------

enum class PeakState { Ready, Building, Failed };
// The whole file's peaks (from 0 s at out->rate: 1000/s for a file up to 10 s, ~380/s from REAPER's
// peak file for a longer one, sound_peaks_plan.h). Building: call again on a later frame (the file is
// read on a reader thread; a file nobody asks for ~2 s is dropped from the wait); `out` is filled
// only when Ready, `length` (seconds) once the file opened. Paths are matched ignoring case and
// slashes. UI thread only.
PeakState RequestPeaks(const std::string& path, PeakData* out, double* length = nullptr);
// 13-54 review: the date + size of `path` its cached peaks were read from (zero when they are not
// Ready), without copying them (the part trims' key, PartTrimsStale). UI thread only.
FileVersion PeaksFileVersion(const std::string& path);
// 13-57: the waveform of [view_start, view_start + view_len) source seconds over `columns` pixels, one
// column per pixel (wave_tiles_plan.h): a view up to kWaveDetailMaxSpan long is read from the file's
// samples at screen resolution (on a reader, in tiles kept in a small LRU; the coarse peaks, or the
// zoom level next to it, show meanwhile: `out->detail` is false until then), signed min / max per
// column joined to the one before, or the samples as a line when a pixel is shorter than a sample.
// False while the file's peaks are not Ready (RequestPeaks says whether they are being read). UI
// thread only; call it each frame the waveform shows.
bool ReadWaveColumns(const std::string& path, double view_start, double view_len, int columns, WaveColumns* out);
// Unload: the reader threads stop and are joined, their sources deleted.
void QuitSoundPeaks();
// The file's length in seconds (0 when it does not open). Cached.
double SourceFileLength(const std::string& path);
// 13-51 review: the file's date and size (both zero when it cannot be read), for a caller that keeps
// what it read of the file (the Sound panel) and asks again at most once a second.
FileVersion SourceFileVersion(const std::string& path);
// Drops the cached length of `path` (the file changed on disk: its length is read again).
void ForgetSourceFileLength(const std::string& path);
// Bumped each time a cached length is dropped (by ForgetSourceFileLength or the peak cache's check).
uint64_t SourceLengthGeneration();

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
    // 13-49 review (Item): what a placed copy of a library Sound leaves out (it plays its file plain):
    // a take with FX, a pitch, a playrate other than 1, a pan; the active take's channel mode (I_CHANMODE).
    bool                     drops_take_fx = false, drops_pitch = false, drops_rate = false, drops_pan = false;
    int                      chan_mode = 0;
};
// Item: the item of that GUID in the current project (take markers, then the chunk's slices);
// File: asset.last_path. False (ok = false) when the source is not found.
bool ReadSoundMedia(const SoundSource& source, SoundMedia* out);
// Where a File Sound's file is when it is not at its last path (the User Library's search: Media
// Explorer databases, another project item playing it); "" = not found. Set by library_user_ui, so
// the panel, Edit and audition find the file the missing check finds (Antho 2026-10-13).
void SetFileSoundResolver(std::function<std::string(const SoundSource&)> resolver);
// The file a variation plays: Takes mode = that take's file, else the active take's.
std::string VariationPath(const SoundMedia& media, const SoundData& data, int variation);
// The source's file ("" when not found).
std::string SoundSourcePath(const SoundSource& source);
// The facts of an item (as ReadSoundMedia's Item branch); false for a null item.
bool ReadItemMedia(MediaItem* item, SoundMedia* out);
// The gain an item plays its audio at, as REAPER shows it on the item: its volume x its active take's
// volume (a REAPER "Normalize items" sets the take's), in dB; -60 for a silent item.
double ItemGainDb(MediaItem* item);
// The same with a given take's volume (Takes mode: each take is a variation at its own gain).
double ItemTakeGainDb(MediaItem* item, MediaItem_Take* take);

}  // namespace rav

#endif  // _WIN32
