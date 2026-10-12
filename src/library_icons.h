// SPDX-License-Identifier: MIT
//
// The Sound Library's icons, drawn as vectors with ImDrawList from the mock-up's 16x16 SVG
// icons (maquette-sound-library-v11.html, ICON and ICON_SPK), so they stay crisp at any size or
// DPI. The default ImGui font has no such symbols. The types (Switch / Random / Blend / Sound),
// the Switch Group (a folder) and its value (a dot), the speaker (Media Explorer auto-play),
// the Tagging strip's event marks (glyph()).

#pragma once

#include "sound_model.h"

#include <imgui.h>

#include "ui_theme.h"

namespace rav {

// Draws the icon of `t` in the square at `p` of side `s`, stroked with `col`.
void DrawNodeTypeGlyph(ImDrawList* dl, ImVec2 p, float s, SoundNodeType t, ImU32 col);

// Every Library icon, by number (ui::IconFn's `which`).
enum LibraryGlyph : int {
    kGlyphSound = 0,
    kGlyphRandom,
    kGlyphBlend,
    kGlyphSwitch,
    kGlyphGroup,    // a Switch Group: a folder
    kGlyphValue,    // a Switch Group value: a dot
    kGlyphSpeaker,  // auto-play
    // 13-8: the Tagging strip's mark under an event (the mock-up's glyph(), 10x10 viewBox), each
    // in the mock-up's own colour (the `col` passed is ignored for these).
    kGlyphEvPopulated,  // sound placed by Populate: a filled blue disc
    kGlyphEvAttached,   // sound attached by hand: a filled amber diamond
    kGlyphEvSpread,     // Spread copy: an outlined light-blue diamond
    kGlyphEvDeleted,    // placed sound deleted by hand: a grey ring
    kGlyphEvMissing,    // sound missing in the last Populate: a red cross
};

// Draws glyph `which` (LibraryGlyph) in the square at `p` of side `s` with `col`. A ui::IconFn.
void DrawLibraryGlyph(ImDrawList* dl, ImVec2 p, float s, int which, ImU32 col);

// Draws a Tagging strip mark (kGlyphEv*) centred on `c`, `s` px wide (the mock-up's 9 px).
void DrawEventGlyph(ImDrawList* dl, ImVec2 c, float s, int which);

// The glyph of a node type.
int GlyphOfType(SoundNodeType t);

// A ui::Icon of `which` (ink: the mock-up's --muted), for IconButton / IconMenuItem / Segmented.
ui::Icon LibraryIcon(int which);
ui::Icon TypeIcon(SoundNodeType t);

}  // namespace rav
