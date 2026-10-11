// SPDX-License-Identifier: MIT
//
// Sound peaks (13-7b, REAPER): a file's mono peaks for waveforms and silence detection.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-7b
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32

#include <string>

#include "sound_model.h"

namespace rav {

void InitSoundPeaks(void* (*get_func)(const char* name));
// Peaks of `path` over [start, start + length) source seconds at `rate` peaks/s. False when unread.
bool ReadPeaks(const std::string& path, double start, double length, double rate, PeakData* out);

}  // namespace rav

#endif  // _WIN32
