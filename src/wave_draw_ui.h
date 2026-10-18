// SPDX-License-Identifier: MIT
//
// 13-71: the one waveform drawer. Every waveform of the tool (the Media Explorer preview, the Sound
// panel, Sound Edit's lanes, its Selection panel's pieces) draws its peaks through DrawWaveLanes: one
// lane per channel of the file (stereo = L above R, as REAPER; WaveLanesFor), one lane for a Sound
// forced to mono, from the columns ReadWaveColumns / ColumnsFromPeaks give (wave_tiles_plan.h). Each
// caller keeps its own look by saying how a column is inked (WaveInkFn: colour, gain, envelope,
// clip colour); the lanes, their zero lines and separators, the columns and the sample line are drawn
// here only, so a change to how waveforms look is made once.

#pragma once

#include <functional>

#include <imgui.h>

#include "sound_model.h"
#include "wave_tiles_plan.h"

namespace rav {

// One layer drawn at a column (or a line segment): its colour, its gain (peaks clamped to full scale at
// it), then an envelope (a fade: 0..1, applied after the clamp). `clip`: when not 0, the part of the
// peaks past full scale at `g` is drawn in this colour (a column: over the lane's edge; a segment: the
// whole segment).
struct WaveInk {
    ImU32  col = 0;
    double g = 1.0;
    double e = 1.0;
    ImU32  clip = 0;
};

constexpr int kWaveMaxInks = 4;

// The inks of one column, or one line segment (`line`), at `pos` (columns from the view's left edge,
// its centre); `peak` = its max |sample| over the lanes drawn, before any gain. Writes up to
// kWaveMaxInks inks into `out`, drawn in order; returns how many (0 = nothing drawn there).
using WaveInkFn = std::function<int(double pos, double peak, bool line, WaveInk* out)>;

struct WaveDrawStyle {
    float pad = 3.0f;     // inside each lane, above and below its peaks (px)
    float px = 1.0f;      // a column's width (px)
    float line_w = 1.0f;  // the sample line's width (px)
    ImU32 zero = 0;       // each lane's zero line (0 = none), drawn under the peaks
    ImU32 split = 0;      // a 1 px line between two lanes (0 = none), drawn over them
};

// Draws `wc`'s lanes (WaveLanesFor(mono, wc.series)) from x = `x0` (column i at x0 + i * px) between
// y0 and y1 (each lane (y1 - y0) / lanes tall). Columns with nothing in them are skipped. Returns the
// lanes drawn.
int DrawWaveLanes(ImDrawList* dl, const WaveColumns& wc, MonoMode mono, float x0, float y0, float y1,
                  const WaveDrawStyle& st, const WaveInkFn& ink);

}  // namespace rav
