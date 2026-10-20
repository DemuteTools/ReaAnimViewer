// SPDX-License-Identifier: MIT
//
// The Sound Library's icons, drawn as vectors with ImDrawList from the mock-up's 16x16 SVG
// icons (maquette-sound-library-v11.html, ICON and ICON_SPK), so they stay crisp at any size or
// DPI. The default ImGui font has no such symbols. The types (Switch / Random / Blend / Sound),
// the Switch Group (a folder) and its value (a dot), the speaker (Media Explorer auto-play),
// the Tagging strip's event marks (glyph()), and the tool icons (cross, duplicate, undo, plus, lock, gear,
// check, fold, open arrow) of the Tagging panel.

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
    kGlyphLink,         // 13-77a: a library linked to a .ravlib
};

// Draws glyph `which` (LibraryGlyph) in the square at `p` of side `s` with `col`. A ui::IconFn.
void DrawLibraryGlyph(ImDrawList* dl, ImVec2 p, float s, int which, ImU32 col);

// 13-77a: the mockup's link glyph (ICON.link, a chain of two links), `s` px wide, stroked with `col`.
void DrawLinkGlyph(ImDrawList* dl, ImVec2 p, float s, ImU32 col);

// Draws a Tagging strip mark (kGlyphEv*) centred on `c`, `s` px wide (the mock-up's 9 px).
void DrawEventGlyph(ImDrawList* dl, ImVec2 c, float s, int which);

// The glyph of a node type.
int GlyphOfType(SoundNodeType t);

// A ui::Icon of `which` (ink: the mock-up's --muted), for IconButton / IconMenuItem / Segmented.
ui::Icon LibraryIcon(int which);
ui::Icon TypeIcon(SoundNodeType t);
// 13-7 (audit A5 / D): a type's icon in the tree and headers: one ink for every container
// (#a8c2ff, ui::kAccentInk, the mockup's .trow.sw .ti / .tib), muted for a Sound -- instead of
// a colour per type.
ui::Icon TypeIconInk(SoundNodeType t);

// 13-14 (finishing): the small tool icons the Tagging panel and the Sounds section draw (the default
// font has no symbols), shared as ui::IconFn glyphs. Each keeps its old 1x drawing: a radius in the
// mockup's 14 px icon square, scaled with the square (so with the screen's DPI).
enum ToolGlyph : int {
    kToolCross = 0,   // remove (r 4)
    kToolCrossSmall,  // remove a row (r 3.5)
    kToolDuplicate,
    kToolUndo,
    kToolPlus,
    kToolLockOpen,
    kToolLockClosed,
    kToolFoldOpen,    // a fold triangle, down
    kToolFoldClosed,  //   right
    kToolGear,
    kToolCheck,       // ok
    kToolOpenArrow,   // a diagonal arrow up-right (open the Library view)
};

// Draws tool glyph `which` (ToolGlyph) in the square at `p` of side `s` with `col`. A ui::IconFn.
void DrawToolGlyph(ImDrawList* dl, ImVec2 p, float s, int which, ImU32 col);
// The same centred on `c` at radius `r` (a glyph drawn inside a custom row: the preset tick, its padlock).
void DrawToolGlyphAt(ImDrawList* dl, ImVec2 c, float r, int which, ImU32 col);
// A ui::Icon of tool glyph `which`, ink `col` (the mockup's --muted by default).
ui::Icon ToolIcon(int which, ImU32 col = ui::kMuted);

}  // namespace rav
