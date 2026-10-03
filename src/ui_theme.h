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

}  // namespace ui
}  // namespace rav

#endif  // _WIN32
