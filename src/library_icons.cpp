// SPDX-License-Identifier: MIT
//
// See library_icons.h. Each shape transcribes the mock-up's SVG path in its 16x16 viewBox.

#include "library_icons.h"

#include <algorithm>
#include <cmath>

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

int GlyphOfType(SoundNodeType t)
{
    switch (t) {
    case SoundNodeType::Switch: return kGlyphSwitch;
    case SoundNodeType::Random: return kGlyphRandom;
    case SoundNodeType::Blend: return kGlyphBlend;
    default: return kGlyphSound;
    }
}

ui::Icon LibraryIcon(int which)
{
    ui::Icon i;
    i.fn = &DrawLibraryGlyph;
    i.which = which;
    i.col = ui::kMuted;
    return i;
}

ui::Icon TypeIcon(SoundNodeType t)
{
    return LibraryIcon(GlyphOfType(t));
}

ui::Icon TypeIconInk(SoundNodeType t)
{
    ui::Icon i = TypeIcon(t);
    if (t != SoundNodeType::Sound) i.col = ui::kAccentInk;
    return i;
}

void DrawLibraryGlyph(ImDrawList* dl, ImVec2 p, float s, int which, ImU32 col)
{
    const float k = s / 16.0f;
    auto P = [&](float x, float y) { return ImVec2(p.x + x * k, p.y + y * k); };
    switch (which) {
    case kGlyphRandom: DrawNodeTypeGlyph(dl, p, s, SoundNodeType::Random, col); break;
    case kGlyphBlend: DrawNodeTypeGlyph(dl, p, s, SoundNodeType::Blend, col); break;
    case kGlyphSwitch: DrawNodeTypeGlyph(dl, p, s, SoundNodeType::Switch, col); break;
    case kGlyphGroup: {
        // M1.5 4.5a1 1 0 0 1 1-1h3.2l1.4 1.5h6.4a1 1 0 0 1 1 1v6.5a1 1 0 0 1-1 1h-11a1 1 0 0 1-1-1z
        const float r = 1.0f * k, pi = 3.14159265f;
        dl->PathArcTo(P(2.5f, 4.5f), r, pi, 1.5f * pi, 4);
        dl->PathLineTo(P(5.7f, 3.5f));
        dl->PathLineTo(P(7.1f, 5.0f));
        dl->PathArcTo(P(13.5f, 6.0f), r, 1.5f * pi, 2.0f * pi, 4);
        dl->PathArcTo(P(13.5f, 12.5f), r, 0.0f, 0.5f * pi, 4);
        dl->PathArcTo(P(2.5f, 12.5f), r, 0.5f * pi, pi, 4);
        dl->PathStroke(col, ImDrawFlags_Closed, std::max(1.0f, 1.2f * k));
        break;
    }
    case kGlyphValue:
        // circle cx8 cy8 r2.4
        dl->AddCircleFilled(P(8.0f, 8.0f), std::max(1.5f, 2.4f * k), col);
        break;
    case kGlyphSpeaker: {
        // M2.5 6h2.5L8.5 3v10L5 10H2.5z (filled), then two arcs (3.5 and 6 around the cone)
        dl->AddRectFilled(P(2.5f, 6.0f), P(5.0f, 10.0f), col);  // the box (the outline is concave: two convex parts)
        const ImVec2 cone[4] = {P(5.0f, 6.0f), P(8.5f, 3.0f), P(8.5f, 13.0f), P(5.0f, 10.0f)};
        dl->AddConvexPolyFilled(cone, 4, col);
        const float w = std::max(1.0f, 1.2f * k);
        dl->PathArcTo(P(8.05f, 8.0f), 3.5f * k, -0.796f, 0.796f, 8);
        dl->PathStroke(col, ImDrawFlags_None, w);
        dl->PathArcTo(P(7.915f, 8.0f), 6.0f * k, -0.775f, 0.775f, 10);
        dl->PathStroke(col, ImDrawFlags_None, w);
        break;
    }
    case kGlyphEvPopulated:
    case kGlyphEvAttached:
    case kGlyphEvSpread:
    case kGlyphEvDeleted:
    case kGlyphEvMissing: DrawEventGlyph(dl, ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 9.0f / 16.0f, which); break;
    default: DrawNodeTypeGlyph(dl, p, s, SoundNodeType::Sound, col); break;
    }
}

void DrawEventGlyph(ImDrawList* dl, ImVec2 c, float s, int which)
{
    const float k = s / 10.0f;  // viewBox 0 0 10 10, centred on c
    auto P = [&](float x, float y) { return ImVec2(c.x + (x - 5.0f) * k, c.y + (y - 5.0f) * k); };
    switch (which) {
    case kGlyphEvPopulated:  // circle r4 fill #6f97ff
        dl->AddCircleFilled(P(5, 5), 4.0f * k, IM_COL32(0x6F, 0x97, 0xFF, 0xFF), 16);
        break;
    case kGlyphEvAttached: {  // M5 .8 9.2 5 5 9.2 .8 5z fill #e3b25a
        const ImVec2 d[4] = {P(5, 0.8f), P(9.2f, 5), P(5, 9.2f), P(0.8f, 5)};
        dl->AddConvexPolyFilled(d, 4, IM_COL32(0xE3, 0xB2, 0x5A, 0xFF));
        break;
    }
    case kGlyphEvSpread: {  // M5 1.2 8.8 5 5 8.8 1.2 5z stroke #8fb0ff 1.4
        const ImVec2 d[4] = {P(5, 1.2f), P(8.8f, 5), P(5, 8.8f), P(1.2f, 5)};
        dl->AddPolyline(d, 4, IM_COL32(0x8F, 0xB0, 0xFF, 0xFF), ImDrawFlags_Closed, std::max(1.0f, 1.4f * k));
        break;
    }
    case kGlyphEvDeleted:  // circle r3.4 stroke #8b919c 1.3
        dl->AddCircle(P(5, 5), 3.4f * k, IM_COL32(0x8B, 0x91, 0x9C, 0xFF), 16, std::max(1.0f, 1.3f * k));
        break;
    case kGlyphEvMissing: {  // M2 2l6 6M8 2 2 8 stroke #f06c6c 1.8
        const ImU32 col = IM_COL32(0xF0, 0x6C, 0x6C, 0xFF);
        const float w = std::max(1.0f, 1.8f * k);
        dl->AddLine(P(2, 2), P(8, 8), col, w);
        dl->AddLine(P(8, 2), P(2, 8), col, w);
        break;
    }
    default: break;
    }
}

// ---- 13-14 (finishing): the tool glyphs (were the Tagging panel's own) ----------------------------

namespace {

constexpr float kPi = 3.14159265f;

void IconCross(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 1.4f);
    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col, 1.4f);
}

void IconCheck(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    const ImVec2 pts[3] = {ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.3f, c.y + r * 0.7f), ImVec2(c.x + r, c.y - r * 0.7f)};
    dl->AddPolyline(pts, 3, col, ImDrawFlags_None, 1.6f);
}

void IconDuplicate(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    dl->AddRect(ImVec2(c.x - r, c.y - r * 0.4f), ImVec2(c.x + r * 0.4f, c.y + r), col, 1.5f);
    dl->AddRect(ImVec2(c.x - r * 0.4f, c.y - r), ImVec2(c.x + r, c.y + r * 0.4f), col, 1.5f);
}

void IconLock(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool closed)
{
    dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.1f), ImVec2(c.x + r, c.y + r), col, 1.5f);
    const ImVec2 sc(c.x, c.y - r * 0.1f);
    const float  sr = r * 0.62f;
    if (closed) {
        dl->PathArcTo(sc, sr, kPi, 2.0f * kPi, 10);
    } else {
        dl->PathArcTo(ImVec2(sc.x - r * 0.0f, sc.y - r * 0.25f), sr, kPi, 1.75f * kPi, 10);
    }
    dl->PathStroke(col, ImDrawFlags_None, 1.5f);
}

void IconPlus(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, 1.6f);
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, 1.6f);
}

// A fold chevron: down when open, right when folded (a category's row, a type's rows).
void IconFold(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool open)
{
    if (open)
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.5f), ImVec2(c.x + r, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.6f), col);
    else
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.5f, c.y - r), ImVec2(c.x - r * 0.5f, c.y + r), ImVec2(c.x + r * 0.6f, c.y), col);
}

void IconGear(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * kPi / 4.0f;
        dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.55f, c.y + std::sin(a) * r * 0.55f),
                    ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), col, 2.0f);
    }
    dl->AddCircle(c, r * 0.6f, col, 16, 1.6f);
}

void IconUndo(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    const float k = r / 5.0f;  // drawn 5 px wide at 1x
    dl->PathArcTo(ImVec2(c.x, c.y + 1.5f * k), r, kPi, kPi * 2.45f, 14);
    dl->PathStroke(col, ImDrawFlags_None, 1.5f);
    const ImVec2 s(c.x - r, c.y + 1.5f * k);
    dl->AddTriangleFilled(ImVec2(s.x - 3.2f * k, s.y - 1.0f * k), ImVec2(s.x + 3.2f * k, s.y - 1.0f * k),
                          ImVec2(s.x, s.y + 3.5f * k), col);
}

// The open-library arrow: a diagonal arrow pointing up-right, `r` the half side of its square.
void IconOpenArrow(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    const float  s = 2.0f * r;
    const ImVec2 p(c.x - r, c.y - r);
    const float  t = std::max(1.0f, ui::Dp(1.5f));
    const ImVec2 a(p.x + s * 0.2f, p.y + s * 0.8f), b(p.x + s * 0.8f, p.y + s * 0.2f);
    dl->AddLine(a, b, col, t);
    dl->AddLine(b, ImVec2(b.x - s * 0.4f, b.y), col, t);
    dl->AddLine(b, ImVec2(b.x, b.y + s * 0.4f), col, t);
}

// Each glyph's radius in the 14 px square (the old 1x drawing's).
float ToolGlyphRadius(int which)
{
    switch (which) {
    case kToolCrossSmall: return 3.5f;
    case kToolDuplicate:
    case kToolLockOpen:
    case kToolLockClosed: return 4.5f;
    case kToolUndo: return 5.0f;
    case kToolGear: return 6.0f;
    case kToolOpenArrow: return 7.0f;  // the whole square
    default: return 4.0f;
    }
}

}  // namespace

void DrawToolGlyphAt(ImDrawList* dl, ImVec2 c, float r, int which, ImU32 col)
{
    switch (which) {
    case kToolCross:
    case kToolCrossSmall: IconCross(dl, c, r, col); break;
    case kToolDuplicate: IconDuplicate(dl, c, r, col); break;
    case kToolUndo: IconUndo(dl, c, r, col); break;
    case kToolPlus: IconPlus(dl, c, r, col); break;
    case kToolLockOpen: IconLock(dl, c, r, col, false); break;
    case kToolLockClosed: IconLock(dl, c, r, col, true); break;
    case kToolFoldOpen: IconFold(dl, c, r, col, true); break;
    case kToolFoldClosed: IconFold(dl, c, r, col, false); break;
    case kToolGear: IconGear(dl, c, r, col); break;
    case kToolCheck: IconCheck(dl, c, r, col); break;
    case kToolOpenArrow: IconOpenArrow(dl, c, r, col); break;
    default: break;
    }
}

void DrawToolGlyph(ImDrawList* dl, ImVec2 p, float s, int which, ImU32 col)
{
    DrawToolGlyphAt(dl, ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), ToolGlyphRadius(which) * s / 14.0f, which, col);
}

ui::Icon ToolIcon(int which, ImU32 col)
{
    return ui::Icon{DrawToolGlyph, which, col};
}

}  // namespace rav
