// SPDX-License-Identifier: MIT
//
// Tagging view, the Dear ImGui side (Epic 10, story 10-3), in the DM-XYZ-Pad theme
// (ui_theme.h), after the validated mock-up v7 (ux-designs/epic-10-auto-tagging):
//   - the curve strip under the 3D view (strip_view.h, shared with Video view's shot strip):
//     one notify row per rule with its markers, then the selected rule's conditions, one
//     lane each: the curve (bright where its condition holds), the rule-colour shading where
//     the whole rule holds, the threshold line and the dashed re-arm line (both draggable);
//   - the panel on the right: a fixed header (item, preset, roles, item options, the rules),
//     the inspector of the selected rule (scrolls), a fixed footer (Apply, the selection).
// Every edit is written at once (tagging_session.h): one REAPER undo point per gesture; a
// drag previews live and writes once on release, Esc cancels it.

#pragma once

#ifdef _WIN32

namespace rav {

constexpr float kTaggingStripMinHeight = 150.0f;
constexpr float kTaggingGap = 8.0f;  // margins to the window's edges, the view and each other

// The strip's height (its top edge drags it; kept in ExtState), between the minimum and
// 60 % of the client height.
float TaggingStripHeight(float client_h);
// The panel's width plus its gaps (its left edge drags it, 280-600 px, kept in ExtState),
// 0 outside Tagging view.
float TaggingPanelFootprint(int client_w);

void DrawTaggingStrip(float x, float y, float w, float h);
void DrawTaggingPanel(float x, float y, float w, float h);

// True while a drag or a typed value runs in the strip or the panel (Esc goes to it).
bool TaggingGestureActive();
// The view changes or the viewer closes: a running drag is written as shown (one undo point).
void TaggingEndGestures();

}  // namespace rav

#endif  // _WIN32
