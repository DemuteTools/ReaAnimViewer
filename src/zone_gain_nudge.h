// SPDX-License-Identifier: MIT
//
// 13-74 (Antho 2026-10-19): the Sound Edit window's "gain of the selected zones" up / down, a RAV action (REAPER has
// none for it): two rows of the Shortcuts popup's Sound Edit group, Ctrl+Shift+wheel up / down over the waveform by
// default (a key works too, one notch per press). The pure rules, host-tested (tests/zone_gain_nudge_test.cpp):
//
// - One notch = 1 dB (kGainNudgeStepDb), as REAPER's item volume nudges ("Item: Nudge items volume +1dB / -1dB").
// - It moves every selected zone's own gain (EditZone::db: what its gain handle and the Selection panel's Gain row
//   edit), each clamped as that row (-60..+24 dB, 0.1 dB steps) notch by notch: a zone at +24 stays there while the
//   others still rise, and the first notch down lowers it at once (no "overshoot" to wind back).
// - Notches in a row on the same Sound, timeline and selection, each within kGainNudgeMergeS of the one before, are
//   ONE gesture: one undo point. The live draft carries them (as a drag does) and is entered once the wheel rests that
//   long, or at once when anything else happens (a click, another key, another Sound shown, the Edit window closed).

#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "sound_zones.h"

namespace rav {

constexpr double kGainNudgeStepDb = 1.0;
constexpr double kGainNudgeMergeS = 0.5;
constexpr double kGainNudgeMinDb = -60.0, kGainNudgeMaxDb = 24.0;  // the Gain row's range (sound_zones ClampDb)

// One gain value moved by `notches` (signed), clamped at each notch, in 0.1 dB.
inline double GainNudged(double db, int notches, double step = kGainNudgeStepDb)
{
    double v = db;
    const int dir = notches > 0 ? 1 : -1;
    for (int k = 0; k < std::abs(notches); ++k) v = std::clamp(std::round((v + dir * step) * 10.0) / 10.0, kGainNudgeMinDb, kGainNudgeMaxDb);
    return v;
}

// Every value moved (GainNudged).
inline std::vector<double> GainsNudged(const std::vector<double>& db, int notches, double step = kGainNudgeStepDb)
{
    std::vector<double> out;
    out.reserve(db.size());
    for (double v : db) out.push_back(GainNudged(v, notches, step));
    return out;
}

// The gesture being merged: its Sound, timeline and selection, the selected zones' gains now, its last notch's time.
struct GainNudgeRun {
    bool                on = false;
    std::string         uid;
    int                 tl = -1;
    ZoneSel             sel;
    std::vector<double> db;      // per sel entry (a Gap's: unused)
    double              last_t = 0.0;
    int                 notches = 0;  // the net notches of the gesture (its toast)
};

// Does a notch at `now` on (uid, tl, sel) continue the run (else the run is entered first and a new one begins)?
inline bool GainNudgeContinues(const GainNudgeRun& r, const std::string& uid, int tl, const ZoneSel& sel, double now)
{
    return r.on && r.uid == uid && r.tl == tl && r.sel == sel && now >= r.last_t && now - r.last_t <= kGainNudgeMergeS;
}

// Is the run due to be entered at `now` (the wheel rested long enough)?
inline bool GainNudgeDue(const GainNudgeRun& r, double now)
{
    return r.on && now - r.last_t > kGainNudgeMergeS;
}

}  // namespace rav
