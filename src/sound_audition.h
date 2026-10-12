// SPDX-License-Identifier: MIT
//
// Sound audition (13-7b, REAPER): plays a node of a library.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-7b
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.
//
// 13-7b (decision 2): one press = ONE preview register (PlayPreview) playing ONE mixing source
// that owns a file source per voice and mixes them with each voice's delay, slice and gain.
// Seams for 13-7c: PlayVoices / AuditionFile.

#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

namespace rav {

void InitSoundAudition(void* (*get_func)(const char* name));
// Stops any audition playing.
void StopAudition();

// ---- 13-7b additions ----------------------------------------------------------------------------

struct PlayVoice {
    std::string path;
    double      start_s = 0.0;   // in the file
    double      length_s = 0.0;  // <= 0 = to the file's end
    double      delay_s = 0.0;   // silence before it
    double      gain = 1.0;      // linear
    double      fade_in_s = 0.0;   // Antho 2026-10-12: the variation's fades (FadeGainAt)
    double      fade_out_s = 0.0;
    int         fade_in_shape = 0;
    int         fade_out_shape = 0;
};
// Plays the voices together (the previous audition stops first). `tag` names what plays (the
// node panel's uid, ...). False, with `why`, when nothing can play (no PlayPreview, no voice read).
bool PlayVoices(const std::vector<PlayVoice>& voices, const std::string& tag, std::string* why = nullptr);
// One stretch of one file (length <= 0 = to its end).
bool AuditionFile(const std::string& path, double start_s, double length_s, const std::string& tag = "file",
                  double gain = 1.0, double fade_in_s = 0.0, double fade_out_s = 0.0, int fade_in_shape = 0,
                  int fade_out_shape = 0);
// True while an audition plays; frees it once it has played to its end.
bool AuditionPlaying();
// Seconds played into the current audition (0 when none).
double AuditionPosition();
// The tag of the playing audition ("" when none).
const std::string& AuditionTag();

}  // namespace rav

#endif  // _WIN32
