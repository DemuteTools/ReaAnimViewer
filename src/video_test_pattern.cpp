// SPDX-License-Identifier: MIT
//
// See video_test_pattern.h. The pattern is a list of filled rectangles computed once
// (BuildRects) and painted either by GL scissored clears or by a CPU fill, so both
// paths give the same picture.

#include "video_test_pattern.h"

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rav {
namespace {

struct Rect {
    int x, y, w, h;           // top-left origin, pixels
    unsigned char r, g, b;
};

constexpr int    kCells         = 16;
constexpr double kBarPeriodSec  = 4.0;
constexpr int    kMaxRects      = 3 + kCells;

// Background, sliding bar, code band, then one rectangle per code cell.
int BuildRects(int w, int h, double project_time, long long frame_index, Rect* out)
{
    int n = 0;
    out[n++] = Rect{0, 0, w, h, 255, 128, 0};

    const int band_h = h / 8;

    // Sliding bar below the band: position from project time only.
    double phase = project_time / kBarPeriodSec;
    phase -= std::floor(phase);  // 0..1, also for negative times
    const int bar_w = (std::max)(2, w / 40);
    const int bar_x = static_cast<int>(phase * static_cast<double>(w - bar_w));
    out[n++] = Rect{bar_x, band_h, bar_w, h - band_h, 20, 24, 32};

    out[n++] = Rect{0, 0, w, band_h, 128, 128, 128};

    // Inset of each code bar inside its cell, so bar edges are countable.
    const int gap = (w / kCells) / 12 + 1;
    const unsigned code = static_cast<unsigned>(frame_index & 0xFFFF);
    for (int i = 0; i < kCells; ++i) {
        const int x0 = i * w / kCells;
        const int x1 = (i + 1) * w / kCells;
        const bool one = ((code >> (kCells - 1 - i)) & 1u) != 0;
        const unsigned char v = one ? 255 : 0;
        out[n++] = Rect{x0 + gap, gap, (x1 - x0) - 2 * gap, band_h - 2 * gap, v, v, v};
    }
    return n;
}

}  // namespace

long long VideoFrameIndex(double project_time, double frame_rate)
{
    if (!(frame_rate > 0.0) || !(project_time > 0.0)) return 0;
    const double f = std::floor(project_time * frame_rate + 0.5);
    if (!(f < 9.0e18)) return 0;  // absurd input (also NaN): never overflow the cast
    return static_cast<long long>(f);
}

void DrawTestPatternGl(int width, int height, double project_time, long long frame_index)
{
    if (width <= 0 || height <= 0) return;
    Rect rects[kMaxRects];
    const int n = BuildRects(width, height, project_time, frame_index, rects);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < n; ++i) {
        const Rect& r = rects[i];
        if (r.w <= 0 || r.h <= 0) continue;
        // GL's origin is bottom-left; the readback flips rows, so convert y here.
        glScissor(r.x, height - (r.y + r.h), r.w, r.h);
        glClearColor(r.r / 255.0f, r.g / 255.0f, r.b / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}

void DrawTestPatternCpu(unsigned char* pixels, int width, int height, int row_bytes, double project_time,
                        long long frame_index)
{
    if (!pixels || width <= 0 || height <= 0 || row_bytes < width * 4) return;
    Rect rects[kMaxRects];
    const int n = BuildRects(width, height, project_time, frame_index, rects);
    for (int i = 0; i < n; ++i) {
        const Rect& r = rects[i];
        const int x0 = (std::max)(0, r.x);
        const int y0 = (std::max)(0, r.y);
        const int x1 = (std::min)(width, r.x + r.w);
        const int y1 = (std::min)(height, r.y + r.h);
        if (x1 <= x0 || y1 <= y0) continue;
        // LICE pixel = 0xAARRGGBB, bytes in memory B,G,R,A.
        const uint32_t px = 0xFF000000u | (uint32_t(r.r) << 16) | (uint32_t(r.g) << 8) | uint32_t(r.b);
        for (int y = y0; y < y1; ++y) {
            unsigned char* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(row_bytes);
            for (int x = x0; x < x1; ++x) std::memcpy(row + static_cast<size_t>(x) * 4, &px, 4);
        }
    }
}

}  // namespace rav
