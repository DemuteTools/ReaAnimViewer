// SPDX-License-Identifier: MIT
//
// The Sound Library's type icons (Switch / Random / Blend / Sound), drawn as vectors with
// ImDrawList from the mock-up's 16x16 SVG icons (maquette-sound-library-v11.html, ICON),
// so they stay crisp at any size or DPI. The default ImGui font has no such symbols.

#pragma once

#include "sound_model.h"

#include <imgui.h>

namespace rav {

// Draws the icon of `t` in the square at `p` of side `s`, stroked with `col`.
void DrawNodeTypeGlyph(ImDrawList* dl, ImVec2 p, float s, SoundNodeType t, ImU32 col);

}  // namespace rav
