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
float MenuArrowRoom() { return Dp(14.0f); }  // a MenuButton's chevron, after its label
}  // namespace

float MenuButtonWidth(const char* label)
{
    return ButtonWidth(label) + MenuArrowRoom();
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
        if (cut && has_extra) ShowTip((text + "\n" + extra).c_str());
        else ShowTip(cut ? text.c_str() : extra);
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
    const float  ty = p.y + ImGui::GetStyle().FramePadding.y;
    float        row_h = ImGui::GetFrameHeight();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    // 13-14 (C4.2): beside its field, a label too wide for the column wraps onto a second line (the
    // mockup's .k): the first line on the field's text line, the row as high as both.
    const flow::MeasureFn measure = [](const char* b, const char* e) { return ImGui::CalcTextSize(b, e).x; };
    const size_t brk = s.beside ? flow::TwoLineBreak(text, s.label_w, measure) : 0;
    if (brk > 0) {
        size_t end = brk;
        while (end > 0 && text[end - 1] == ' ') --end;
        const float line = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(p.x, ty), col, text.c_str(), text.c_str() + end);
        const bool cut = EllipsisText(dl, ImVec2(p.x, ty + line), p.x + s.label_w, col, text.substr(brk));
        row_h = std::max(row_h, ty - p.y + 2.0f * line);
        if (cut && !ImGui::IsAnyItemActive() &&
            ImGui::IsMouseHoveringRect(p, ImVec2(p.x + s.label_w, p.y + row_h)) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_ForTooltip))
            ShowTip(text.c_str());
    } else {
        EllipsisTextTip(dl, ImVec2(p.x, ty), p.x + s.label_w, col, text);
    }
    ImGui::Dummy(ImVec2(std::max(1.0f, s.beside ? s.field_dx - sp : s.label_w), row_h));
    if (s.beside) ImGui::SameLine(0.0f, sp);
    return s.field_w;
}

float UnitWidth(const char* unit)
{
    // 13-7 (B6): the .u unit, 10 px, 6 px after its field (as DragNumber draws it).
    return (unit && unit[0]) ? Dp(6.0f) + TextSizeIn(FontTiny(), unit).x : 0.0f;
}

namespace {

bool FittedButtonBody(const char* label, const ImVec2& size)
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
        ShowTip(text.c_str());
    return pressed;
}

}  // namespace

bool FittedButton(const char* label, const ImVec2& size)
{
    // 13-14 (B2): inside a disabled region, the mockup's disabled skin at full opacity.
    const bool skin = PushDisabledButtonSkin();
    const bool pressed = FittedButtonBody(label, size);
    PopDisabledButtonSkin(skin);
    return pressed;
}

bool MenuButton(const char* label, bool enabled, const char* visible, const Icon* icon)
{
    const float room = icon && icon->fn ? IconRoom() : 0.0f;
    if (!enabled) ImGui::BeginDisabled();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float       arrow = MenuArrowRoom();
    const std::string text = visible ? std::string(visible) : std::string(label, VisibleEnd(label));
    const float       nat = ImGui::CalcTextSize(text.c_str(), nullptr, false).x + 2.0f * st.FramePadding.x + arrow + room;
    const float       avail = ImGui::GetContentRegionAvail().x;
    const float       w = WindowAutoSizes() ? nat : std::max(ImGui::GetFrameHeight() + arrow + room, std::min(nat, avail));
    // The button (raised fill, stroke edge) with no label of its own: the label and the chevron
    // are drawn over it, the label ellipsized before the chevron.
    ImGui::PushStyleColor(ImGuiCol_Button, Col(kRaised));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(kHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(kAccentSoft));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStroke));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    // 13-14 (B2 / C3.2): disabled = the mockup's skin (raised or, in a toolbar, transparent; faint
    // label, icon and chevron) at full opacity, not ImGui's fade.
    const bool  skin = PushDisabledButtonSkin();
    const bool  dis = ItemDisabled();
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    const bool r = ImGui::Button(label, ImVec2(w, 0.0f));
    ImGui::PopStyleColor();
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    if (room > 0.0f) {
        Icon ic = *icon;
        if (dis) ic.col = kFaint;
        PaintIconAt(dl, ic, mn.x + st.FramePadding.x, mn.y, mx.y - mn.y);
    }
    const bool cut = EllipsisText(dl, ImVec2(mn.x + st.FramePadding.x + room, mn.y + (mx.y - mn.y - ImGui::GetTextLineHeight()) * 0.5f),
                                  mx.x - arrow - Dp(2.0f), ink, text);
    if (cut && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ShowTip(text.c_str());
    // The chevron (down).
    const ImVec2 c(mx.x - Dp(9.0f), (mn.y + mx.y) * 0.5f);
    const float  rr = Dp(3.5f);
    dl->AddTriangleFilled(ImVec2(c.x - rr, c.y - rr * 0.6f), ImVec2(c.x + rr, c.y - rr * 0.6f), ImVec2(c.x, c.y + rr * 0.8f),
                          ImGui::GetColorU32(dis ? kFaint : kMuted));
    PopDisabledButtonSkin(skin);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (!enabled) ImGui::EndDisabled();
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
    // 13-14 (B2 / C3.2): the disabled skin (a toolbar's .tbb.dis: transparent, faint text and icon).
    const bool skin = PushDisabledButtonSkin();
    const bool dis = ItemDisabled();
    // The button with no label of its own: icon and label are drawn over it, centred together.
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    const bool pressed = ImGui::Button(label, ImVec2(w, size.y));
    ImGui::PopStyleColor();
    const ImVec2      a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const std::string text(label, VisibleEnd(label));
    const float       cw = room + TextWidth(label);
    const float       x = a.x + std::max(st.FramePadding.x, (w - cw) * 0.5f);
    ImDrawList*       dl = ImGui::GetWindowDrawList();
    Icon              ic = icon;
    if (dis) ic.col = kFaint;
    PaintIconAt(dl, ic, x, a.y, b.y - a.y);
    const bool cut = EllipsisText(dl, ImVec2(x + room, a.y + (b.y - a.y - ImGui::GetTextLineHeight()) * 0.5f),
                                  b.x - st.FramePadding.x, ink, text);
    if (cut && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ShowTip(text.c_str());
    PopDisabledButtonSkin(skin);
    if (solid) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }
    return pressed;
}

namespace {

// 13-7 (B16): a menu row's icon (.pop .mi .ico): 15 px, 8 px before its label.
float MenuIconPx() { return Dp(15.0f); }
float MenuIconRoom() { return IconRoom(MenuIconPx(), Dp(8.0f)); }

// The label pushed right by the icon's room (spaces), its "##id" part kept.
std::string IconPadded(const char* label)
{
    const float sp = std::max(1.0f, ImGui::CalcTextSize(" ").x);
    const int   n = static_cast<int>(MenuIconRoom() / sp + 0.999f);
    return std::string(static_cast<size_t>(std::max(1, n)), ' ') + label;
}

// 13-14 (B16): ImGui::MenuItem with the mockup's hint (.pop .mi .hint: 11 px faint, at the right of
// the row, where ImGui puts its shortcut column) and a faint disabled row (.pop .mi[disabled]).
bool MenuRow(const char* label, const char* shortcut, bool selected, bool enabled)
{
    const bool   hint = shortcut && shortcut[0];
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  right = p.x + ImGui::GetContentRegionAvail().x;
    if (!enabled) {
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);  // MenuItem's own BeginDisabled: no fade
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kFaint));
    }
    if (hint) ImGui::PushStyleColor(ImGuiCol_TextDisabled, Col(IM_COL32(0, 0, 0, 0)));  // its column, drawn below
    const bool r = ImGui::MenuItem(label, shortcut, selected, enabled);
    if (hint) ImGui::PopStyleColor();
    if (!enabled) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    if (hint) {
        const ImGuiWindow* win = ImGui::GetCurrentWindowRead();
        const float        mark_w = std::floor(ImGui::GetFontSize() * 1.20f);  // ImGui's check mark column
        const float        gap = win ? static_cast<float>(win->DC.MenuColumns.Spacing) : ImGui::GetStyle().ItemSpacing.x;
        ImFont*            f = FontSmall();
        const ImVec2       ts = TextSizeIn(f, shortcut);
        const float        line = ImGui::GetTextLineHeight();
        AddTextIn(ImGui::GetWindowDrawList(), f, ImVec2(right - mark_w - gap - ts.x, std::floor(p.y + (line - ts.y) * 0.5f)),
                  ImGui::GetColorU32(kFaint), shortcut);
    }
    return r;
}

}  // namespace

bool IconMenuItem(const Icon& icon, const char* label, const char* shortcut, bool selected, bool enabled)
{
    ImDrawList*       dl = ImGui::GetWindowDrawList();
    const ImVec2      p = ImGui::GetCursorScreenPos();
    const std::string padded = IconPadded(label);
    const bool        r = MenuRow(padded.c_str(), shortcut, selected, enabled);
    Icon              ic = icon;
    if (!enabled) ic.col = kFaint;
    PaintIconAt(dl, ic, p.x, p.y, ImGui::GetTextLineHeight(), MenuIconPx());
    return r;
}

bool IconBeginMenu(const Icon& icon, const char* label, bool enabled)
{
    ImDrawList*       dl = ImGui::GetWindowDrawList();  // the parent menu's: BeginMenu may open a child window
    const ImVec2      p = ImGui::GetCursorScreenPos();
    const std::string padded = IconPadded(label);
    Icon              ic = icon;
    if (!enabled) ic.col = kFaint;
    PaintIconAt(dl, ic, p.x, p.y, ImGui::GetTextLineHeight(), MenuIconPx());
    if (!enabled) {
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kFaint));
    }
    const bool open = ImGui::BeginMenu(padded.c_str(), enabled);
    if (!enabled) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    return open;
}

bool MenuEntry(const char* label, const char* shortcut, bool selected, bool enabled)
{
    return MenuRow(label, shortcut, selected, enabled);
}

}  // namespace ui
}  // namespace rav

#endif  // _WIN32
