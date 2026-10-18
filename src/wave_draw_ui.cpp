// SPDX-License-Identifier: MIT
//
// 13-71: the one waveform drawer (wave_draw_ui.h).

#include "wave_draw_ui.h"

#include <algorithm>
#include <cmath>

namespace rav {

int DrawWaveLanes(ImDrawList* dl, const WaveColumns& wc, MonoMode mono, float x0, float y0, float y1,
                  const WaveDrawStyle& st, const WaveInkFn& ink)
{
    if (!dl || !(y1 > y0)) return 0;
    const WaveLanes L = WaveLanesFor(mono, wc.series);
    const float     lane_h = (y1 - y0) / static_cast<float>(L.n);
    const float     hh = WaveLaneAt(y0, y1, L.n, st.pad, 0).hh;  // the same in every lane
    float           mid[kWaveMaxLanes];
    for (int l = 0; l < L.n; ++l) mid[l] = WaveLaneAt(y0, y1, L.n, st.pad, l).mid;
    if (st.zero)
        for (int l = 0; l < L.n; ++l) {
            const float y = std::floor(mid[l]) + 0.5f;
            dl->AddLine(ImVec2(x0, y), ImVec2(x0 + st.px * static_cast<float>(std::max(1, wc.columns)), y), st.zero);
        }
    WaveInk inks[kWaveMaxInks];
    auto    Y = [&](int l, double v) { return mid[l] - static_cast<float>(v) * hh; };
    if (wc.line && wc.pt_x.size() >= 2) {
        // A pixel shorter than a sample: the samples joined by a line, each segment inked as its column.
        for (size_t k = 0; k + 1 < wc.pt_x.size(); ++k) {
            double peak = 0.0;
            for (int l = 0; l < L.n; ++l) {
                const std::vector<float>& v = wc.pt_v[L.series[l]];
                if (v.size() != wc.pt_x.size()) continue;
                peak = std::max({peak, std::fabs(static_cast<double>(v[k])), std::fabs(static_cast<double>(v[k + 1]))});
            }
            const int   n = std::min(kWaveMaxInks, ink((wc.pt_x[k] + wc.pt_x[k + 1]) * 0.5, peak, true, inks));
            const float xa = x0 + static_cast<float>(wc.pt_x[k]) * st.px, xb = x0 + static_cast<float>(wc.pt_x[k + 1]) * st.px;
            for (int l = 0; l < L.n; ++l) {
                const std::vector<float>& v = wc.pt_v[L.series[l]];
                if (v.size() != wc.pt_x.size()) continue;
                for (int j = 0; j < n; ++j) {
                    const WaveInk& I = inks[j];
                    const double   a = v[k] * I.g, b = v[k + 1] * I.g;
                    const bool     over = I.clip && (std::fabs(a) > 1.0 || std::fabs(b) > 1.0);
                    dl->AddLine(ImVec2(xa, Y(l, std::clamp(a, -1.0, 1.0) * I.e)), ImVec2(xb, Y(l, std::clamp(b, -1.0, 1.0) * I.e)),
                                over ? I.clip : I.col, st.line_w);
                }
            }
        }
    } else {
        for (int i = 0; i < wc.columns; ++i) {
            const size_t c = static_cast<size_t>(i);
            double       peak = -1.0;
            for (int l = 0; l < L.n; ++l)
                if (WaveColumnHas(wc, L.series[l], c))
                    peak = std::max({peak, static_cast<double>(wc.hi[L.series[l]][c]), -static_cast<double>(wc.lo[L.series[l]][c])});
            if (peak < 0.0) continue;  // nothing in this column on any lane
            const int   n = std::min(kWaveMaxInks, ink(static_cast<double>(i) + 0.5, peak, false, inks));
            const float x = std::floor(x0 + static_cast<float>(i) * st.px);
            for (int l = 0; l < L.n; ++l) {
                const int s = L.series[l];
                if (!WaveColumnHas(wc, s, c)) continue;
                for (int j = 0; j < n; ++j) {
                    const WaveInk& I = inks[j];
                    const double   hi = wc.hi[s][c] * I.g, lo = wc.lo[s][c] * I.g;
                    const float    ya = std::floor(Y(l, std::clamp(hi, -1.0, 1.0) * I.e));
                    const float    yb = std::floor(Y(l, std::clamp(lo, -1.0, 1.0) * I.e)) + 1.0f;
                    dl->AddRectFilled(ImVec2(x, ya), ImVec2(x + 1.0f, yb), I.col);
                    if (!I.clip) continue;
                    if (hi > 1.0) {
                        const float over = std::min(hh, static_cast<float>(hi - 1.0) * hh);
                        dl->AddRectFilled(ImVec2(x, std::floor(mid[l] - hh)), ImVec2(x + 1.0f, std::floor(mid[l] - hh + over) + 1.0f), I.clip);
                    }
                    if (lo < -1.0) {
                        const float over = std::min(hh, static_cast<float>(-lo - 1.0) * hh);
                        dl->AddRectFilled(ImVec2(x, std::floor(mid[l] + hh - over)), ImVec2(x + 1.0f, std::floor(mid[l] + hh) + 1.0f), I.clip);
                    }
                }
            }
        }
    }
    if (st.split)
        for (int l = 1; l < L.n; ++l) {
            const float y = std::floor(y0 + lane_h * static_cast<float>(l));
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + st.px * static_cast<float>(std::max(1, wc.columns)), y + 1.0f), st.split);
        }
    return L.n;
}

}  // namespace rav
