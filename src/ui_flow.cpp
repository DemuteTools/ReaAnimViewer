// SPDX-License-Identifier: MIT
//
// See ui_flow.h.

#include "ui_flow.h"

#ifdef _WIN32

#include <algorithm>
#include <cstring>

#include <imgui_internal.h>  // ImGuiWindow flags (a window that sizes itself)

#include "ui_flow_math.h"

namespace rav {
namespace ui {

namespace {

const char* VisibleEnd(const char* label)
{
    const char* e = std::strstr(label, "##");
    return e ? e : label + std::strlen(label);
}

}  // namespace

float TextWidth(const char* label)
{
    return label ? ImGui::CalcTextSize(label, nullptr, true).x : 0.0f;
}

float ButtonWidth(const char* label)
{
    return TextWidth(label) + 2.0f * ImGui::GetStyle().FramePadding.x;
}

namespace {
constexpr float kMenuArrowRoom = 14.0f;  // a MenuButton's chevron, after its label
}  // namespace

float MenuButtonWidth(const char* label)
{
    return ButtonWidth(label) + kMenuArrowRoom;
}

float LineRight()
{
    return ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
}

bool WindowAutoSizes()
{
    const ImGuiWindow* w = ImGui::GetCurrentWindowRead();
    return w && (w->Flags & (ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup)) != 0 &&
           !(w->Flags & ImGuiWindowFlags_ChildWindow);
}

bool SameLineIfFits(float next_w)
{
    const bool fits = WindowAutoSizes() ||
                      flow::FitsAfter(ImGui::GetItemRectMax().x, ImGui::GetStyle().ItemSpacing.x, next_w, LineRight());
    if (fits) ImGui::SameLine();
    return fits;
}

bool EllipsisTextTip(ImDrawList* dl, ImVec2 p, float max_x, ImU32 col, const std::string& text, const char* extra)
{
    const bool cut = EllipsisText(dl, p, max_x, col, text);
    const bool has_extra = extra && extra[0];
    // Never over a drag (a drag-and-drop preview, a fader): the tooltip would replace or chase it.
    if ((cut || has_extra) && !ImGui::IsAnyItemActive() && !ImGui::GetDragDropPayload() &&
        ImGui::IsMouseHoveringRect(p, ImVec2(std::max(p.x, max_x), p.y + ImGui::GetTextLineHeight())) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_ForTooltip)) {
        if (cut && has_extra) ImGui::SetTooltip("%s\n%s", text.c_str(), extra);
        else ImGui::SetTooltip("%s", cut ? text.c_str() : extra);
    }
    return cut;
}

bool FitText(const std::string& text, ImU32 col, float width)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  room = std::max(1.0f, width > 0.0f ? width : ImGui::GetContentRegionAvail().x);
    const float  nat = ImGui::CalcTextSize(text.c_str()).x;
    const bool   cut = EllipsisTextTip(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + ImGui::GetStyle().FramePadding.y),
                                       p.x + room, col, text);
    ImGui::Dummy(ImVec2(std::max(1.0f, std::min(room, nat + 1.0f)), ImGui::GetFrameHeight()));
    return cut;
}

float LabelRow(const char* label, float field_min, float label_col, ImU32 col)
{
    return LabelRowText(std::string(label, VisibleEnd(label)), field_min, label_col, col);
}

float LabelRowText(const std::string& text, float field_min, float label_col, ImU32 col)
{
    const float  sp = ImGui::GetStyle().ItemSpacing.x;
    const float  avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const flow::LabelSplit s =
        flow::SplitLabelRow(avail, ImGui::CalcTextSize(text.c_str(), nullptr, false).x, label_col, field_min, sp);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    EllipsisTextTip(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + ImGui::GetStyle().FramePadding.y), p.x + s.label_w, col,
                    text);
    ImGui::Dummy(ImVec2(std::max(1.0f, s.beside ? s.field_dx - sp : s.label_w), ImGui::GetFrameHeight()));
    if (s.beside) ImGui::SameLine(0.0f, sp);
    return s.field_w;
}

float UnitWidth(const char* unit)
{
    return (unit && unit[0]) ? 4.0f + ImGui::CalcTextSize(unit).x : 0.0f;
}

bool FittedButton(const char* label, const ImVec2& size)
{
    const float nat = ButtonWidth(label);
    if (WindowAutoSizes()) return ImGui::Button(label, size);
    const float avail = ImGui::GetContentRegionAvail().x;
    // A negative width is "the room left minus |size.x|", as in ImGui; resolved here so it shortens too.
    const float want = size.x < 0.0f ? std::max(1.0f, avail + size.x) : size.x;
    float       w = want > 0.0f ? want : nat;
    if (w > avail) w = std::max(ImGui::GetFrameHeight(), avail);  // never narrower than a square
    if (w >= nat) return ImGui::Button(label, ImVec2(want > 0.0f ? w : 0.0f, size.y));
    // Too narrow for its label: the button keeps its id and colours, its label is ellipsized.
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    const bool pressed = ImGui::Button(label, ImVec2(w, size.y));
    ImGui::PopStyleColor();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float  px = ImGui::GetStyle().FramePadding.x;
    const std::string text(label, VisibleEnd(label));
    EllipsisText(ImGui::GetWindowDrawList(), ImVec2(a.x + px, a.y + (b.y - a.y - ImGui::GetTextLineHeight()) * 0.5f),
                 b.x - px, ink, text);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text.c_str());
    return pressed;
}

bool MenuButton(const char* label, bool enabled, const char* visible, const Icon* icon)
{
    const float room = icon && icon->fn ? IconRoom() : 0.0f;
    if (!enabled) ImGui::BeginDisabled();
    const ImGuiStyle& st = ImGui::GetStyle();
    const std::string text = visible ? std::string(visible) : std::string(label, VisibleEnd(label));
    const float       nat = ImGui::CalcTextSize(text.c_str(), nullptr, false).x + 2.0f * st.FramePadding.x + kMenuArrowRoom + room;
    const float       avail = ImGui::GetContentRegionAvail().x;
    const float       w = WindowAutoSizes() ? nat : std::max(ImGui::GetFrameHeight() + kMenuArrowRoom + room, std::min(nat, avail));
    // The button (raised fill, stroke edge) with no label of its own: the label and the chevron
    // are drawn over it, the label ellipsized before the chevron.
    ImGui::PushStyleColor(ImGuiCol_Button, Col(kRaised));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(kHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(kAccentSoft));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStroke));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    const bool r = ImGui::Button(label, ImVec2(w, 0.0f));
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (!enabled) ImGui::EndDisabled();
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    if (room > 0.0f) {
        if (!enabled) ImGui::BeginDisabled();  // the icon fades with the button
        PaintIconAt(dl, *icon, mn.x + st.FramePadding.x, mn.y, mx.y - mn.y);
        if (!enabled) ImGui::EndDisabled();
    }
    const bool cut = EllipsisText(dl, ImVec2(mn.x + st.FramePadding.x + room, mn.y + (mx.y - mn.y - ImGui::GetTextLineHeight()) * 0.5f),
                                  mx.x - kMenuArrowRoom - 2.0f, ink, text);
    if (cut && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text.c_str());
    // The chevron (down).
    const ImVec2 c(mx.x - 9.0f, (mn.y + mx.y) * 0.5f);
    const float  rr = 3.5f;
    dl->AddTriangleFilled(ImVec2(c.x - rr, c.y - rr * 0.6f), ImVec2(c.x + rr, c.y - rr * 0.6f), ImVec2(c.x, c.y + rr * 0.8f),
                          enabled ? kMuted : kFaint);
    return r;
}

float IconButtonWidth(const char* label)
{
    return ButtonWidth(label) + IconRoom();
}

float IconMenuButtonWidth(const char* label)
{
    return MenuButtonWidth(label) + IconRoom();
}

bool IconButton(const Icon& icon, const char* label, const ImVec2& size, bool solid)
{
    const ImGuiStyle& st = ImGui::GetStyle();
    const float       room = IconRoom();
    const float       nat = IconButtonWidth(label);
    const float       avail = ImGui::GetContentRegionAvail().x;
    const float       want = size.x < 0.0f ? std::max(1.0f, avail + size.x) : size.x;
    float             w = std::max(want, nat);
    if (!WindowAutoSizes() && w > avail) w = std::max(ImGui::GetFrameHeight(), avail);  // never narrower than a square
    if (solid) {
        ImGui::PushStyleColor(ImGuiCol_Button, Col(kRaised));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(kHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(kAccentSoft));
        ImGui::PushStyleColor(ImGuiCol_Border, Col(kStroke));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    }
    // The button with no label of its own: icon and label are drawn over it, centred together.
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    const bool pressed = ImGui::Button(label, ImVec2(w, size.y));
    ImGui::PopStyleColor();
    if (solid) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }
    const ImVec2      a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const std::string text(label, VisibleEnd(label));
    const float       cw = room + TextWidth(label);
    const float       x = a.x + std::max(st.FramePadding.x, (w - cw) * 0.5f);
    ImDrawList*       dl = ImGui::GetWindowDrawList();
    PaintIconAt(dl, icon, x, a.y, b.y - a.y);
    const bool cut = EllipsisText(dl, ImVec2(x + room, a.y + (b.y - a.y - ImGui::GetTextLineHeight()) * 0.5f),
                                  b.x - st.FramePadding.x, ink, text);
    if (cut && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text.c_str());
    return pressed;
}

namespace {

// The label pushed right by the icon's room (spaces), its "##id" part kept.
std::string IconPadded(const char* label)
{
    const float sp = std::max(1.0f, ImGui::CalcTextSize(" ").x);
    const int   n = static_cast<int>(IconRoom() / sp + 0.999f);
    return std::string(static_cast<size_t>(std::max(1, n)), ' ') + label;
}

}  // namespace

bool IconMenuItem(const Icon& icon, const char* label, const char* shortcut, bool selected, bool enabled)
{
    ImDrawList*       dl = ImGui::GetWindowDrawList();
    const ImVec2      p = ImGui::GetCursorScreenPos();
    const std::string padded = IconPadded(label);
    const bool        r = ImGui::MenuItem(padded.c_str(), shortcut, selected, enabled);
    if (!enabled) ImGui::BeginDisabled();
    PaintIconAt(dl, icon, p.x, p.y, ImGui::GetTextLineHeight());
    if (!enabled) ImGui::EndDisabled();
    return r;
}

bool IconBeginMenu(const Icon& icon, const char* label, bool enabled)
{
    ImDrawList*       dl = ImGui::GetWindowDrawList();  // the parent menu's: BeginMenu may open a child window
    const ImVec2      p = ImGui::GetCursorScreenPos();
    const std::string padded = IconPadded(label);
    if (!enabled) ImGui::BeginDisabled();
    PaintIconAt(dl, icon, p.x, p.y, ImGui::GetTextLineHeight());
    if (!enabled) ImGui::EndDisabled();
    return ImGui::BeginMenu(padded.c_str(), enabled);
}

}  // namespace ui
}  // namespace rav

#endif  // _WIN32
