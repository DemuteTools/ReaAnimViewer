// SPDX-License-Identifier: MIT
//
// 13-33: "Expand all" / "Collapse all", the same control in every Library panel that holds several
// foldable elements (the node tree, the node panel's sections, the Switch Groups list): two discreet
// ghost icon buttons, ⊞ then ⊟ (the glyphs of the Sound assignment window's Expand all / Collapse
// all), 22 px each, each off while it has nothing to do. View only: no undo point, the project is
// never touched.

#pragma once

#ifdef _WIN32

#include <imgui.h>

namespace rav {
namespace ui {

// The pair on the cursor's line: +1 = Expand all clicked, -1 = Collapse all, 0 = neither.
int   FoldAllButtons(const char* id, bool can_expand, bool can_collapse);
float FoldAllButtonsWidth();

// The foldable ui::Sections drawn between BeginFoldScope(id) and EndFoldScope() form one scope;
// FoldScopeBar(id) draws the pair for it, right-aligned on a row of its own, when the scope drew at
// least two foldable sections last frame (nothing otherwise), and a click opens / folds them all on the
// next frame. `id` is a plain key (not ImGui's ID stack). Scopes do not nest.
void BeginFoldScope(const char* id);
// `complete` false: the frame stopped drawing (a gesture ran), the scope keeps last frame's count.
void EndFoldScope(bool complete = true);
bool FoldScopeBar(const char* id);
// BeginSection's hook for a foldable section (its open state at `open_id` in the window's storage):
// a pending Expand / Collapse all of the current scope sets it, and the scope counts it.
void NoteFoldableSection(ImGuiID open_id, bool* open);

}  // namespace ui
}  // namespace rav

#endif  // _WIN32
