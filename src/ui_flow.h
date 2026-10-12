// SPDX-License-Identifier: MIT
//
// Layout that adapts to the width it is given, by construction (13-7a / 13-7b / 13-7c): the
// shared helpers every panel uses instead of fixed widths. Measure before placing; whatever does
// not fit wraps to the next line, ellipsizes (its full text in a tooltip) or shrinks -- nothing is
// ever clipped by its column. The arithmetic is ui_flow_math.h (pure, host-tested).
//
// The theme's own widgets follow the same rules (ui_theme.h): SolidButton / PrimaryButton shrink
// to the room left and ellipsize their label, Segmented shares or wraps its options, DragNumber
// shrinks its field so its unit always shows, a Section's content ends at its card's inner edge.
// A window that sizes itself to its content (a popup, a tooltip, a toast) is never shrunk.
// Fixed pixel widths are only ever minimums. ImGui only; call between NewFrame and Render.

#pragma once

#ifdef _WIN32

#include <imgui.h>

#include <string>

#include "ui_theme.h"

namespace rav {
namespace ui {

// The label column of a labelled row (LabelRow): the mockup's .row > .k, 80 px. 13-14 (N3): at the
// screen's DPI (a DpPx reads as the scaled float).
inline constexpr DpPx kLabelCol{80.0f};

// The visible part of a label ("Text##id" -> "Text") measured.
float TextWidth(const char* label);
// A button's natural width (its visible label plus the frame padding).
float ButtonWidth(const char* label);
// A MenuButton's natural width (its label plus room for its chevron).
float MenuButtonWidth(const char* label);
// The x where the current line ends (the content region's right edge, cards included).
float LineRight();
// True when the current window sizes itself to its content: widgets keep their natural size.
bool WindowAutoSizes();

// Measure before place: call after an item, before the next one (`next_w` wide). The next item
// goes beside the last one when it fits, else on the next line. True when beside. Measured from
// where the line really ends (a Fader's value readout included, though the track is the last item).
bool SameLineIfFits(float next_w);

// One line of text (aligned with the frame widgets), ellipsized to `width` (0 = the room left),
// its full text as a tooltip when shortened. An item of the width it takes. True when shortened.
bool FitText(const std::string& text, ImU32 col = kText, float width = 0.0f);

// EllipsisText plus the full text as a tooltip while the mouse is over it (draw-list rows).
bool EllipsisTextTip(ImDrawList* dl, ImVec2 p, float max_x, ImU32 col, const std::string& text,
                     const char* extra = nullptr);  // extra: shown in the tooltip too (and alone when not cut)

// A labelled row: the label in the label column (ellipsized, tooltip), the cursor left where the
// field goes -- beside the label when the field keeps `field_min`, else under it the whole width.
// Returns the field's width (pass it to SetNextItemWidth / the widget's width).
// 13-7: a setting's label ("Type", "Volume") is muted, as the mockup's .k.
// 13-14 (C4.2): beside its field, a label wider than its column wraps onto a second line at a space
// ("Default / value"), the first line on the field's text line, the row as high as both lines; with
// no usable space (or still too long) it is ellipsized, its full text as a tooltip.
float LabelRow(const char* label, float field_min, float label_col = kLabelCol, ImU32 col = kMuted);
// The same with literal text (a user-typed name: "##" in it is shown, not an id; kText, a name).
float LabelRowText(const std::string& text, float field_min, float label_col = kLabelCol, ImU32 col = kText);

// The width a unit ("dB") takes after its field (gap included: 6 px, the unit in FontTiny); 0 for none.
float UnitWidth(const char* unit);

// ImGui::Button that never runs past the room left: wider than it (natural or `size.x`), it
// takes the room and its label is ellipsized, the full label as a tooltip. size.x < 0 = ImGui's
// "avail + size.x". The base of SolidButton / PrimaryButton (with the current style colours).
bool FittedButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));

// A SolidButton with a small down arrow after its label (a menu opens on click). Its label is
// ellipsized when the room left is narrower; the arrow always shows.
bool MenuButton(const char* label, bool enabled = true, const char* visible = nullptr,  // visible: literal shown text
                const Icon* icon = nullptr);                                           // icon: drawn before the label

// 13-7: icon + label widgets (the mock-up's buttons and menu rows carry a type icon). Measured
// as icon + text, so SameLineIfFits / the shrink-and-ellipsize rules above hold for them too:
// a narrow button keeps its icon and ellipsizes its label.
float IconButtonWidth(const char* label);      // ButtonWidth + IconRoom
float IconMenuButtonWidth(const char* label);  // MenuButtonWidth + IconRoom
// A FittedButton (solid = SolidButton's colours) with `icon` before its label.
bool IconButton(const Icon& icon, const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f), bool solid = true);
// ImGui::MenuItem / BeginMenu with `icon` before the label (padded right by the icon room).
// 13-14 (B16): the shortcut is the mockup's hint (.pop .mi .hint: 11 px faint, at the row's right);
// a disabled row (enabled = false) is faint (.pop .mi[disabled]), its icon too.
bool IconMenuItem(const Icon& icon, const char* label, const char* shortcut = nullptr, bool selected = false,
                  bool enabled = true);
bool IconBeginMenu(const Icon& icon, const char* label, bool enabled = true);
// 13-14: the same menu row without an icon (ImGui::MenuItem with the hint and the disabled look).
bool MenuEntry(const char* label, const char* shortcut = nullptr, bool selected = false, bool enabled = true);

// The option of the last Segmented under the mouse (-1 = none).
int SegmentedHovered();

}  // namespace ui
}  // namespace rav

#endif  // _WIN32
