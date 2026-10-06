// SPDX-License-Identifier: MIT
//
// The house theme of Demute's tools (DM-XYZ-Pad lib/ui/theme.lua, validated mock-up
// maquette-refonte-ui-v2), laid over Dear ImGui (Story 11-4): neutral greys from #121317
// to #282c34, text #e7e9ee / #8b919c, one accent #5b8cff, radii 10 / 7 / 5, ghost
// buttons (transparent until hovered), segmented controls, thin discreet scrollbar.
//
// ApplyRavTheme sets the live ImGui style once (the whole viewer UI uses it). The
// helpers draw the theme's components that ImGui has no widget for. ImGui only; call
// between NewFrame and Render (except ApplyRavTheme).

#pragma once

#ifdef _WIN32

#include <imgui.h>

#include <string>

namespace rav {
namespace ui {

// ---- palette (theme.lua PALETTE + the mock-up's state colours) -----------------------
constexpr ImU32 kBg           = IM_COL32(0x12, 0x13, 0x17, 0xFF);
constexpr ImU32 kSurface      = IM_COL32(0x19, 0x1B, 0x20, 0xFF);
constexpr ImU32 kRaised       = IM_COL32(0x20, 0x23, 0x2A, 0xFF);
constexpr ImU32 kHover        = IM_COL32(0x28, 0x2C, 0x34, 0xFF);
constexpr ImU32 kStroke       = IM_COL32(0x2B, 0x2F, 0x37, 0xFF);
constexpr ImU32 kStrokeStrong = IM_COL32(0x3A, 0x3F, 0x49, 0xFF);
constexpr ImU32 kFrameHover   = IM_COL32(0x47, 0x4D, 0x59, 0xFF);  // a control under the mouse
constexpr ImU32 kText         = IM_COL32(0xE7, 0xE9, 0xEE, 0xFF);
constexpr ImU32 kMuted        = IM_COL32(0x8B, 0x91, 0x9C, 0xFF);
constexpr ImU32 kFaint        = IM_COL32(0x5D, 0x63, 0x6E, 0xFF);
constexpr ImU32 kAccent       = IM_COL32(0x5B, 0x8C, 0xFF, 0xFF);
constexpr ImU32 kAccentSoft   = IM_COL32(0x5B, 0x8C, 0xFF, 0x29);
constexpr ImU32 kAccentLine   = IM_COL32(0x5B, 0x8C, 0xFF, 0x8C);
constexpr ImU32 kPrimaryHover = IM_COL32(0x6D, 0x99, 0xFF, 0xFF);
constexpr ImU32 kOk           = IM_COL32(0x5F, 0xDD, 0x9E, 0xFF);
constexpr ImU32 kWarn         = IM_COL32(0xFF, 0xD1, 0x66, 0xFF);
// Story 10-3b -- the amber in-place confirm (overwrite / delete) and the Legacy band: its
// outline (kWarn at .45), its action button and that button's text.
constexpr ImU32 kConfirmLine      = (kWarn & ~IM_COL32_A_MASK) | (0x73u << IM_COL32_A_SHIFT);
constexpr ImU32 kConfirmAct       = IM_COL32(0xD9, 0xA1, 0x2B, 0xFF);
constexpr ImU32 kConfirmActHover  = IM_COL32(0xE6, 0xB0, 0x3E, 0xFF);
constexpr ImU32 kConfirmActText   = IM_COL32(0x1A, 0x1A, 0x1A, 0xFF);
constexpr ImU32 kWhite        = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
constexpr ImU32 kFrameStroke  = IM_COL32(0xE7, 0xE9, 0xEE, 0x47);  // the Video view frame's edge (text at .28)

// The curve palette (theme.lua CURVE_PALETTE, the mock-up's shot colours): same
// saturation and lightness, only the hue changes. Shot i takes kCurvePalette[i % 8].
constexpr ImU32 kCurvePalette[8] = {
    IM_COL32(0xDD, 0x5F, 0x5F, 0xFF), IM_COL32(0x5F, 0xDD, 0xDD, 0xFF), IM_COL32(0xDD, 0xDD, 0x5F, 0xFF),
    IM_COL32(0x5F, 0x5F, 0xDD, 0xFF), IM_COL32(0x5F, 0xDD, 0x5F, 0xFF), IM_COL32(0xDD, 0x5F, 0xDD, 0xFF),
    IM_COL32(0xDD, 0x9E, 0x5F, 0xFF), IM_COL32(0x5F, 0x9E, 0xDD, 0xFF),
};

// ---- radii (theme.lua RADIUS_LG / MD / SM) ----------------------------------------------
constexpr float kRadiusLg = 10.0f;
constexpr float kRadiusMd = 7.0f;
constexpr float kRadiusSm = 5.0f;

ImVec4 Col(ImU32 c);

// Lays the theme over the live style (call once after ImGui::CreateContext).
void ApplyRavTheme();

// A segmented control (the mock-up's .seg): `count` options side by side in a dark
// well, the selected one raised. `item_width` 0 = sized to the labels. An option at
// `disabled_index` cannot be picked. Returns true when *selected changed.
bool Segmented(const char* id, const char* const* labels, int count, int* selected,
               float item_width = 0.0f, int disabled_index = -1);

// Buttons: primary = accent fill, solid = raised fill with a stroke edge. Ghost (the
// default ImGui::Button under this theme) needs no helper.
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
bool SolidButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));

// A pill with a coloured dot and a label (the mock-up's .tag with .dot).
void StateTag(ImU32 dot, const char* text);

// A small muted caption (the mock-up's .k label).
void Caption(const char* text);

// Muted, wrapped body text (the mock-up's .sub).
void SubText(const char* text);

// Story 10-3 -- a number field (the mock-up's input.nin), never a slider. Dragging it
// sideways changes the value by `step` per pixel (Shift held: a tenth of it); a click
// without a drag types a value (Enter or a click elsewhere enters it, Esc cancels).
// `*value` is in the field's own (display) units, rounded to `decimals`; `unit` is drawn
// after the number ("" = none). `width` 0 = sized to a typical value.
enum class DragNumberEvent {
    None,
    Live,    // dragging: *value changed this frame (preview it, write nothing)
    Commit,  // released after a drag, or a typed value entered: write *value (one undo point)
    Cancel,  // Esc: *value is back to its value before the gesture
};
DragNumberEvent DragNumber(const char* id, double* value, double step, int decimals, const char* unit,
                           float width = 0.0f);
// True while a DragNumber is dragged or typed into (the viewer routes Esc to it).
bool DragNumberActive();
// Forgets a running drag / typed value (its view went away; the caller ends the gesture).
void DragNumberReset();

// A key cap ("C") drawn at p (top-left), the mock-up's <kbd>. Returns its width.
// `text` 0 = the theme's text colour.
float KeyCap(ImDrawList* dl, ImVec2 p, const char* key, ImU32 text = 0);

// ---- compartments and aligned sentences (spec 10-3-fb-2, made systemic by the
// viewer-options-menu-tabs spec) ------------------------------------------------------------

// Text from p, shortened with "..." (whole glyphs) so it ends before max_x. True when shortened.
bool EllipsisText(ImDrawList* dl, ImVec2 p, float max_x, ImU32 col, const std::string& text);

// A boxed track: its header cell (x0..lx, kRaised) and its lane (lx..x1, lane_fill), filled.
void TrackFill(ImDrawList* dl, float x0, float lx, float x1, float y0, float y1, ImU32 lane_fill);
// A boxed track's edges: the divider between header and lane, one edge around both.
void TrackEdge(ImDrawList* dl, float x0, float lx, float x1, float y0, float y1, ImU32 edge);

// A 3 px colour stripe on the left of a rounded box (p0..p1): the box's own rounded rect
// clipped to the stripe, so it follows the box's corners.
void RuleStripe(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float radius);

// A card: its fill and edge are drawn behind its content once its height is known.
// BeginCard / EndCard around the content; the cursor ends under it, a kCardGap below.
constexpr float kCardPad = 8.0f;
constexpr float kCardGap = 6.0f;
struct Card {
    ImDrawListSplitter split;
    ImVec2             p0;
    float              w = 0.0f;
};
void BeginCard(Card& k);
void EndCard(Card& k, ImU32 fill, ImU32 edge);

// THE SECTION: how every group of settings is shown, in any panel or menu -- a card
// (kSurface on the window's kBg, kStroke edge) headed by its icon and title, a divider under
// the header. Its header folds it (a chevron on the right; the state is kept for the
// session). Always pair with EndSection, open or not:
//     ui::Section s;
//     if (ui::BeginSection(s, "Light", icon)) { ...settings... }
//     ui::EndSection(s);
// A new setting goes inside a section: it then looks like the others with no styling of its own.
struct Section {
    Card card;
};
bool BeginSection(Section& s, const char* title, ImTextureID icon = ImTextureID(), bool foldable = true);
void EndSection(Section& s);

// The "aligned sentence": one phrase per row, its connector word right-aligned in a fixed
// column, every row's chips from one shared edge. A chip that does not fit beside the
// previous one wraps under the row's first chip (a row wraps as a whole, never a lone word).
struct Sentence {
    float x0 = 0.0f;       // the label column's left
    float chips_x = 0.0f;  // the chips' shared left edge
    float right = 0.0f;    // the right edge chips stay within
    bool  first = true;    // no chip on this row yet
};
// The label column's width: as wide as the widest of `labels`, and of `badge` drawn as a
// badge (its pad included; nullptr = none). Each caller passes its own labels.
float SentenceColumnWidth(const char* const* labels, int count, const char* badge = nullptr);
// A sentence whose label column starts at x0, `column_w` wide (SentenceColumnWidth), its
// chips within `right`.
Sentence MakeSentence(float x0, float right, float column_w);
// Starts a row on the cursor's line: its label right-aligned in the column (an item, so it
// can be hovered). `badge`: drawn as a tag (the IF / AND badge).
void SentenceRow(Sentence& s, const char* label, bool badge = false);
// Puts the cursor where the row's next chip (w wide) goes: beside the previous one when it
// fits (`spacing` < 0 = the item spacing), else under the row's first chip.
void SentenceChip(Sentence& s, float w, float spacing = -1.0f);

// The width a DragNumber field_w wide takes with its unit.
float NumberFieldWidth(float field_w, const char* unit);

// Vertical tabs (REAPER's preferences column): `count` labels stacked, `width` wide (0 =
// sized to the labels). The selected row: kAccentSoft fill and an accent bar on its left,
// text kText; the others muted, kHover under the mouse. Returns true when *selected changed.
bool VerticalTabs(const char* id, const char* const* labels, int count, int* selected, float width = 0.0f);

}  // namespace ui
}  // namespace rav

#endif  // _WIN32
