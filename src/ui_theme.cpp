// SPDX-License-Identifier: MIT
//
// See ui_theme.h.

#include "ui_theme.h"

#ifdef _WIN32

#include <algorithm>
#include <cstring>

namespace rav {
namespace ui {

ImVec4 Col(ImU32 c)
{
    return ImGui::ColorConvertU32ToFloat4(c);
}

void ApplyRavTheme()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    // theme.lua COLOURS, one by one.
    c[ImGuiCol_WindowBg]             = Col(kBg);
    c[ImGuiCol_ChildBg]              = Col(kSurface);
    c[ImGuiCol_PopupBg]              = Col(kRaised);
    c[ImGuiCol_Border]               = Col(kStroke);
    c[ImGuiCol_Separator]            = Col(kStroke);
    c[ImGuiCol_SeparatorHovered]     = Col(kStrokeStrong);
    c[ImGuiCol_SeparatorActive]      = Col(kAccentLine);
    c[ImGuiCol_Text]                 = Col(kText);
    c[ImGuiCol_TextDisabled]         = Col(kMuted);
    // Controls (slider tracks, checkboxes, fields) use the strong stroke grey, as
    // theme.lua's switch-off does: at kRaised they vanished into the menu's background.
    c[ImGuiCol_FrameBg]              = Col(kStrokeStrong);
    c[ImGuiCol_FrameBgHovered]       = Col(kFrameHover);
    c[ImGuiCol_FrameBgActive]        = Col(kFrameHover);
    c[ImGuiCol_TitleBg]              = Col(kBg);
    c[ImGuiCol_TitleBgActive]        = Col(kSurface);
    c[ImGuiCol_TitleBgCollapsed]     = Col(kBg);
    c[ImGuiCol_Button]               = Col(IM_COL32(0, 0, 0, 0));  // ghost until hovered
    c[ImGuiCol_ButtonHovered]        = Col(kHover);
    c[ImGuiCol_ButtonActive]         = Col(kAccentSoft);
    c[ImGuiCol_Header]               = Col(kRaised);
    c[ImGuiCol_HeaderHovered]        = Col(kHover);
    c[ImGuiCol_HeaderActive]         = Col(kAccentSoft);
    c[ImGuiCol_CheckMark]            = Col(kAccent);
    c[ImGuiCol_SliderGrab]           = Col(kAccent);
    c[ImGuiCol_SliderGrabActive]     = Col(kAccent);
    c[ImGuiCol_Tab]                  = Col(kSurface);
    c[ImGuiCol_TabHovered]           = Col(kHover);
    c[ImGuiCol_TabSelected]          = Col(kRaised);
    c[ImGuiCol_TextSelectedBg]       = Col(kAccentSoft);
    c[ImGuiCol_ResizeGrip]           = Col(IM_COL32(0, 0, 0, 0));
    c[ImGuiCol_ResizeGripHovered]    = Col(kStrokeStrong);
    c[ImGuiCol_ResizeGripActive]     = Col(kAccentLine);
    // The discreet scrollbar (Story 11-4 AC): no track, a grab in the stroke grey that
    // only stands out under the mouse.
    c[ImGuiCol_ScrollbarBg]          = Col(IM_COL32(0, 0, 0, 0));
    c[ImGuiCol_ScrollbarGrab]        = Col(kStroke);
    c[ImGuiCol_ScrollbarGrabHovered] = Col(kStrokeStrong);
    c[ImGuiCol_ScrollbarGrabActive]  = Col(kFaint);

    // Tooltips wait 0.5 s on a still mouse (Antho's feedback): every tooltip is gated by
    // IsItemHovered(ImGuiHoveredFlags_ForTooltip) / SetItemTooltip, which read these.
    s.HoverDelayNormal          = 0.5f;
    s.HoverFlagsForTooltipMouse = ImGuiHoveredFlags_Stationary | ImGuiHoveredFlags_DelayNormal |
                                  ImGuiHoveredFlags_AllowWhenDisabled;

    // theme.lua STYLE_VARS: radii 10 / 7 / 5, padding 10.
    s.WindowRounding    = kRadiusLg;
    s.ChildRounding     = kRadiusLg;
    s.PopupRounding     = kRadiusLg;
    s.FrameRounding     = kRadiusMd;
    s.GrabRounding      = kRadiusSm;
    s.ScrollbarRounding = kRadiusSm;
    s.TabRounding       = kRadiusSm;
    s.WindowPadding     = ImVec2(10.0f, 10.0f);
    s.ScrollbarSize     = 6.0f;
    s.WindowBorderSize  = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.GrabMinSize       = 8.0f;
    s.ItemSpacing       = ImVec2(8.0f, 6.0f);
}

bool Segmented(const char* id, const char* const* labels, int count, int* selected, float item_width,
               int disabled_index)
{
    if (!labels || count <= 0 || !selected) return false;
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = 2.0f;
    const float h = ImGui::GetTextLineHeight() + 8.0f;

    float widths[8] = {};
    const int n = std::min(count, 8);
    float total = pad;
    for (int i = 0; i < n; ++i) {
        widths[i] = item_width > 0.0f ? item_width : ImGui::CalcTextSize(labels[i], nullptr, true).x + 24.0f;
        total += widths[i] + pad;
    }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(total, h + 2.0f * pad);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), kBg, kRadiusMd);
    dl->AddRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), kStroke, kRadiusMd);

    bool changed = false;
    float x = p0.x + pad;
    for (int i = 0; i < n; ++i) {
        const ImVec2 a(x, p0.y + pad);
        const ImVec2 b(x + widths[i], p0.y + pad + h);
        const bool disabled = (i == disabled_index);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (disabled) ImGui::BeginDisabled();
        const bool clicked = ImGui::InvisibleButton("##seg", ImVec2(widths[i], h));
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (disabled) ImGui::EndDisabled();
        ImGui::PopID();
        if (clicked && !disabled && *selected != i) {
            *selected = i;
            changed = true;
        }
        const bool on = (*selected == i);
        if (on) {
            dl->AddRectFilled(a, b, kRaised, kRadiusSm);
            dl->AddRect(a, b, kStrokeStrong, kRadiusSm);
        } else if (hovered && !disabled) {
            dl->AddRectFilled(a, b, kHover, kRadiusSm);
        }
        const ImVec2 ts = ImGui::CalcTextSize(labels[i], nullptr, true);
        const ImU32 ink = disabled ? kFaint : (on ? kText : kMuted);
        const char* end = std::strstr(labels[i], "##");  // the visible part of the label
        dl->AddText(ImVec2(a.x + (widths[i] - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f), ink, labels[i], end);
        x += widths[i] + pad;
    }

    // One item for the whole control, so the layout continues after it.
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(size);
    ImGui::PopID();
    return changed;
}

bool PrimaryButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, Col(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(kPrimaryHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(kAccent));
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kWhite));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool SolidButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, Col(kRaised));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(kHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(kAccentSoft));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStroke));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return pressed;
}

void StateTag(ImU32 dot, const char* text)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float h = ts.y + 6.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(ts.x + 30.0f, h);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), kBg, h * 0.5f);
    dl->AddRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), kStroke, h * 0.5f);
    dl->AddCircleFilled(ImVec2(p0.x + 12.0f, p0.y + h * 0.5f), 3.5f, dot, 12);
    dl->AddText(ImVec2(p0.x + 21.0f, p0.y + 3.0f), kMuted, text);
    ImGui::Dummy(size);
}

void Caption(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void SubText(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

float KeyCap(ImDrawList* dl, ImVec2 p, const char* key, ImU32 text)
{
    const ImVec2 ts = ImGui::CalcTextSize(key);
    const ImVec2 b(p.x + ts.x + 8.0f, p.y + ts.y + 3.0f);
    dl->AddRectFilled(p, b, kBg, 4.0f);
    dl->AddRect(p, b, kStrokeStrong, 4.0f);
    dl->AddText(ImVec2(p.x + 4.0f, p.y + 1.5f), text ? text : kText, key);
    return b.x - p.x;
}

}  // namespace ui
}  // namespace rav

#endif  // _WIN32
