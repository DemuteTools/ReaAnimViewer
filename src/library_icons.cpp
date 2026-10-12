// SPDX-License-Identifier: MIT
//
// See library_icons.h. Each shape transcribes the mock-up's SVG path in its 16x16 viewBox.

#include "library_icons.h"

#include <algorithm>

namespace rav {

void DrawNodeTypeGlyph(ImDrawList* dl, ImVec2 p, float s, SoundNodeType t, ImU32 col)
{
    const float k = s / 16.0f;
    auto P = [&](float x, float y) { return ImVec2(p.x + x * k, p.y + y * k); };
    const float w = std::max(1.0f, 1.3f * k);
    auto line = [&](float x0, float y0, float x1, float y1) { dl->AddLine(P(x0, y0), P(x1, y1), col, w); };
    switch (t) {
    case SoundNodeType::Switch: {
        // M2 8h4l3-4h5  M9 12h5  M6 8l3 4
        const ImVec2 top[4] = {P(2, 8), P(6, 8), P(9, 4), P(14, 4)};
        dl->AddPolyline(top, 4, col, ImDrawFlags_None, w);
        line(9, 12, 14, 12);
        line(6, 8, 9, 12);
        break;
    }
    case SoundNodeType::Random: {
        // rounded square 2.5..13.5 r2, three dice pips r1.1
        dl->AddRect(P(2.5f, 2.5f), P(13.5f, 13.5f), col, 2.0f * k, ImDrawFlags_None, std::max(1.0f, 1.2f * k));
        const float r = std::max(0.8f, 1.1f * k);
        dl->AddCircleFilled(P(5.6f, 5.6f), r, col);
        dl->AddCircleFilled(P(8.0f, 8.0f), r, col);
        dl->AddCircleFilled(P(10.4f, 10.4f), r, col);
        break;
    }
    case SoundNodeType::Blend:
        // three stacked layers
        line(2.5f, 5.5f, 13.5f, 5.5f);
        line(2.5f, 8.0f, 13.5f, 8.0f);
        line(2.5f, 10.5f, 13.5f, 10.5f);
        break;
    default:
        // Sound: a small waveform of five bars
        line(3.0f, 9.5f, 3.0f, 6.5f);
        line(5.5f, 11.0f, 5.5f, 5.0f);
        line(8.0f, 13.0f, 8.0f, 3.0f);
        line(10.5f, 11.0f, 10.5f, 5.0f);
        line(13.0f, 9.5f, 13.0f, 6.5f);
        break;
    }
}

}  // namespace rav
