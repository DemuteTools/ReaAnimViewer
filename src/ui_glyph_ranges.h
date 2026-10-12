// SPDX-License-Identifier: MIT
//
// 13-7: the Unicode blocks the tool UI may draw as text. Dear ImGui's default font
// (ProggyClean) only holds Basic Latin + Latin-1, so any other symbol in a UI string
// (the play triangle, the stop square, the ellipsis, the arrows, the stars...) was drawn
// as ImGui's fallback "?". At start-up the viewer merges Windows system fonts (Segoe UI,
// Segoe UI Symbol, Arial) into the atlas for exactly these ranges; tests/ui_glyphs_test
// scans every string literal under src/ and fails on a code point outside them, so a new
// symbol never silently turns back into "?".
//
// Pure header (no ImGui, no Windows): the values are ImWchar pairs [first, last], ending
// with 0, the layout ImFontConfig::GlyphRanges expects (ImWchar is 16 bits here).

#pragma once

namespace rav {

inline constexpr unsigned short kUiGlyphRanges[] = {
    0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement (multiplication x, middle dot...)
    0x2000, 0x206F,  // General Punctuation (em dash, ellipsis, bullet, single guillemets)
    0x2190, 0x21FF,  // Arrows (right arrow, loop arrows, north-east arrow)
    0x2200, 0x22FF,  // Mathematical Operators (minus sign, midline ellipsis)
    0x2300, 0x23FF,  // Miscellaneous Technical (media controls)
    0x25A0, 0x25FF,  // Geometric Shapes (stop square, play triangle, small down triangle)
    0x2600, 0x26FF,  // Miscellaneous Symbols (black / white star)
    0x2700, 0x27BF,  // Dingbats (check mark, heavy multiplication x)
    0,
};

// True when code point `cp` lies in one of the ranges above.
inline constexpr bool UiGlyphCovered(unsigned long cp) {
    for (int i = 0; kUiGlyphRanges[i] != 0; i += 2)
        if (cp >= kUiGlyphRanges[i] && cp <= kUiGlyphRanges[i + 1]) return true;
    return false;
}

}  // namespace rav
