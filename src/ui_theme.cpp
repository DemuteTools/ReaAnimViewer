// SPDX-License-Identifier: MIT
//
// See ui_theme.h.

#include "ui_theme.h"
#include "ui_drag_number.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

#include <imgui_internal.h>  // a card narrows the window's work rect; next-item width; hovered ids

#include "ui_flow.h"
#include "ui_flow_math.h"

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
    // 13-7: the mockup's select is stroke-strong too; its text fields are kBg with a stroke
    // edge (ui::TextField), its faders a thin track (ui::Fader).
    c[ImGuiCol_FrameBg]              = Col(kStrokeStrong);
    c[ImGuiCol_FrameBgHovered]       = Col(kFrameHover);
    c[ImGuiCol_FrameBgActive]        = Col(kFrameHover);
    c[ImGuiCol_TitleBg]              = Col(kBg);
    c[ImGuiCol_TitleBgActive]        = Col(kSurface);
    c[ImGuiCol_TitleBgCollapsed]     = Col(kBg);
    c[ImGuiCol_Button]               = Col(IM_COL32(0, 0, 0, 0));  // ghost until hovered
    c[ImGuiCol_ButtonHovered]        = Col(kHover);
    c[ImGuiCol_ButtonActive]         = Col(kAccentSoft);
    // 13-7 (B10): a selected row is accent-soft, as the mockup's (ui::SelectRow adds its edge);
    // a menu row under the mouse stays kHover (.pop .mi:hover).
    c[ImGuiCol_Header]               = Col(kAccentSoft);
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
    // only stands out under the mouse (the mockup's scrollbar-color: var(--stroke) transparent).
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
    s.PopupRounding     = kRadiusMd;  // 13-7 (A3): .pop r-md
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
    // 13-7 (A2): the mockup's metrics. FramePadding (8, 3) with the 16 px line of the 12 px body
    // font = the 22 px .btn.sm; ItemSpacing (6, 7) = .row gap 6 and .secx-b gap 7; a checkbox's
    // label 6 px after its box.
    s.FramePadding      = ImVec2(8.0f, 3.0f);
    s.ItemSpacing       = ImVec2(6.0f, 7.0f);
    s.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
}

namespace {
float   g_scale = 1.0f;
int     g_seg_hovered = -1;
FontSet g_fonts;
int     g_section_depth = 0;   // sections open around the current one (a nested one is kBg)
bool    g_selrow_hovered = false;

ImFont* DefaultFont() { return g_fonts.body ? g_fonts.body : ImGui::GetDefaultFont(); }
ImFont* FontOr(ImFont* f, ImFont* fallback) { return f ? f : fallback; }

// The visible part of a label ("Text##id" -> end at "##").
const char* LabelEnd(const char* label)
{
    const char* e = std::strstr(label, "##");
    return e ? e : label + std::strlen(label);
}

// A colour with the current style alpha (faded inside BeginDisabled).
ImU32 Ink(ImU32 c) { return ImGui::GetColorU32(c); }

}  // namespace

void SetUiScale(float scale) { g_scale = (scale >= 0.5f && scale <= 4.0f) ? scale : 1.0f; }
float UiScale() { return g_scale; }
float ControlH() { return Dp(22.0f); }
float FieldH() { return Dp(24.0f); }
float LargeH() { return Dp(26.0f); }
float RowH() { return Dp(28.0f); }
float SectionHeaderH() { return Dp(26.0f); }
float MenuRowH() { return Dp(26.0f); }

void SetFonts(const FontSet& fonts) { g_fonts = fonts; }
ImFont* FontBody()        { return DefaultFont(); }
ImFont* FontSmall()       { return FontOr(g_fonts.small, DefaultFont()); }
ImFont* FontTitle()       { return FontOr(g_fonts.title, DefaultFont()); }
ImFont* FontBig()         { return FontOr(g_fonts.big, FontTitle()); }
ImFont* FontMono()        { return FontOr(g_fonts.mono, DefaultFont()); }
ImFont* FontTiny()        { return FontOr(g_fonts.tiny, FontSmall()); }
ImFont* FontBadge()       { return FontOr(g_fonts.badge, FontTiny()); }
ImFont* FontSemibold()    { return FontOr(g_fonts.semibold, DefaultFont()); }
ImFont* FontCaptionBold() { return FontOr(g_fonts.caption_bold, FontSmall()); }
ImFont* FontHeading()     { return FontOr(g_fonts.heading, FontTitle()); }
ImFont* FontPaneTitle()   { return FontOr(g_fonts.pane_title, FontBig()); }
ImFont* FontMonoSmall()   { return FontOr(g_fonts.mono_small, FontMono()); }
ImFont* FontMonoTiny()    { return FontOr(g_fonts.mono_tiny, FontMonoSmall()); }
ImFont* FontMonoMicro()   { return FontOr(g_fonts.mono_micro, FontMonoTiny()); }

ImFont* Font(FontRole role)
{
    switch (role) {
    case FontRole::Body: return FontBody();
    case FontRole::Small: return FontSmall();
    case FontRole::Title: return FontTitle();
    case FontRole::Big: return FontBig();
    case FontRole::Mono: return FontMono();
    case FontRole::Tiny: return FontTiny();
    case FontRole::Badge: return FontBadge();
    case FontRole::Semibold: return FontSemibold();
    case FontRole::CaptionBold: return FontCaptionBold();
    case FontRole::Heading: return FontHeading();
    case FontRole::PaneTitle: return FontPaneTitle();
    case FontRole::MonoSmall: return FontMonoSmall();
    case FontRole::MonoTiny: return FontMonoTiny();
    case FontRole::MonoMicro: return FontMonoMicro();
    }
    return FontBody();
}

FontScope::FontScope(ImFont* f) { ImGui::PushFont(f ? f : DefaultFont()); }
FontScope::~FontScope() { ImGui::PopFont(); }

ImVec2 TextSizeIn(ImFont* font, const char* text, const char* text_end)
{
    if (!text) return ImVec2(0.0f, 0.0f);
    if (!font) font = DefaultFont();
    if (!text_end) text_end = LabelEnd(text);
    ImVec2 s = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text, text_end);
    s.x = std::floor(s.x + 0.99999f);
    return s;
}

void AddTextIn(ImDrawList* dl, ImFont* font, ImVec2 p, ImU32 col, const char* text, const char* text_end)
{
    if (!dl || !text) return;
    if (!font) font = DefaultFont();
    dl->AddText(font, font->FontSize, p, col, text, text_end ? text_end : LabelEnd(text));
}

int SegmentedHovered() { return g_seg_hovered; }

float IconPx() { return Dp(14.0f); }

float IconRoom() { return IconPx() + Dp(5.0f); }

float IconRoom(float icon_px, float gap) { return icon_px + gap; }

void PaintIconAt(ImDrawList* dl, const Icon& icon, float x, float y, float line_h, float icon_px)
{
    if (!icon.fn || !dl) return;
    const float s = icon_px > 0.0f ? icon_px : IconPx();
    icon.fn(dl, ImVec2(x, y + (line_h - s) * 0.5f), s, icon.which, ImGui::GetColorU32(icon.col));
}

bool Segmented(const char* id, const char* const* labels, int count, int* selected, float item_width,
               int disabled_index, const Icon* icons, const char* const* badges)
{
    g_seg_hovered = -1;
    if (!labels || count <= 0 || !selected) return false;
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // 13-7 (B4): the .seg well (pad 2, gap 2), its options 22 px high, 10 px each side, 13 px icons.
    const float pad = Dp(2.0f);
    const float h = ControlH();
    const float kPadNat = Dp(20.0f), kPadMin = Dp(8.0f);  // an option's room around its label
    const float icon_px = Dp(13.0f);
    const float icon_room = IconRoom(icon_px, Dp(5.0f));
    ImFont*     badge_font = FontBadge();

    // Measure, then lay out within the room left (ui_flow_math: share, then wrap onto rows).
    float     text_w[8] = {}, widths[8] = {}, badge_w[8] = {};
    int       row_of[8] = {};
    const int n = std::min(count, 8);
    for (int i = 0; i < n; ++i) {
        badge_w[i] = badges && badges[i] && badges[i][0] ? Dp(2.0f) + TextSizeIn(badge_font, badges[i]).x : 0.0f;
        text_w[i] = item_width > 0.0f ? std::max(0.0f, item_width - kPadNat)
                                      : TextWidth(labels[i]) + (icons && icons[i].fn ? icon_room : 0.0f) + badge_w[i];
    }
    const float avail = WindowAutoSizes() ? 1e9f : std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const int   rows = flow::LayoutSegments(text_w, n, avail, kPadNat, kPadMin, pad, widths, row_of);
    float       total = 0.0f;  // the widest row
    for (int r = 0, i = 0; r < rows; ++r) {
        float x = pad;
        for (; i < n && row_of[i] == r; ++i) x += widths[i] + pad;
        total = std::max(total, x);
    }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(total, static_cast<float>(rows) * (h + pad) + pad);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), Ink(kBg), kRadiusMd);
    dl->AddRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), Ink(kStroke), kRadiusMd);

    bool  changed = false;
    float x = p0.x + pad;
    for (int i = 0; i < n; ++i) {
        if (i > 0 && row_of[i] != row_of[i - 1]) x = p0.x + pad;
        const float  y = p0.y + pad + static_cast<float>(row_of[i]) * (h + pad);
        const ImVec2 a(x, y);
        const ImVec2 b(x + widths[i], y + h);
        const bool disabled = (i == disabled_index);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (disabled) ImGui::BeginDisabled();
        const bool clicked = ImGui::InvisibleButton("##seg", ImVec2(widths[i], h));
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (disabled) ImGui::EndDisabled();
        ImGui::PopID();
        if (hovered) g_seg_hovered = i;
        if (clicked && !disabled && *selected != i) {
            *selected = i;
            changed = true;
        }
        const bool on = (*selected == i);
        if (on) {
            dl->AddRectFilled(a, b, Ink(kRaised), kRadiusSm);
            dl->AddRect(a, b, Ink(kStrokeStrong), kRadiusSm);
        } else if (hovered && !disabled) {
            dl->AddRectFilled(a, b, Ink(kHover), kRadiusSm);
        }
        const char*       end = std::strstr(labels[i], "##");  // the visible part of the label
        const std::string text = end ? std::string(labels[i], end) : std::string(labels[i]);
        const float       room = icons && icons[i].fn ? icon_room : 0.0f;  // the option's icon, before its label
        const float       tw = TextWidth(labels[i]) + room + badge_w[i];
        const ImU32       ink = Ink(disabled ? kFaint : (on ? kText : (badge_w[i] > 0.0f ? kAccentText : kMuted)));
        const float       ty = a.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
        if (tw <= widths[i] - 4.0f) {
            const float tx = a.x + (widths[i] - tw) * 0.5f;
            if (room > 0.0f) {
                Icon ic = icons[i];
                if (disabled) ic.col = kFaint;
                PaintIconAt(dl, ic, tx, a.y, h, icon_px);
            }
            dl->AddText(ImVec2(tx + room, ty), ink, text.c_str());
            if (badge_w[i] > 0.0f)  // the superscript badge, top-aligned 2 px after the label
                AddTextIn(dl, badge_font, ImVec2(tx + room + TextWidth(labels[i]) + Dp(2.0f), a.y + Dp(2.0f)),
                          Ink(kNote), badges[i]);
        } else if (room > 0.0f && widths[i] - 8.0f >= room) {  // the icon stays, the label is ellipsized after it
            PaintIconAt(dl, icons[i], a.x + 4.0f, a.y, h, icon_px);
            if (EllipsisText(dl, ImVec2(a.x + 4.0f + room, ty), b.x - 4.0f, ink, text) && hovered)
                ImGui::SetTooltip("%s", text.c_str());
        } else if (EllipsisText(dl, ImVec2(a.x + 4.0f, ty), b.x - 4.0f, ink, text) && hovered) {
            ImGui::SetTooltip("%s", text.c_str());
        }
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
    const bool pressed = FittedButton(label, size);
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
    const bool pressed = FittedButton(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return pressed;
}

bool ToggleButton(const char* label, bool on, const ImVec2& size)
{
    if (!on) return SolidButton(label, size);
    ImGui::PushStyleColor(ImGuiCol_Button, Col(kAccentSoft));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(IM_COL32(0x5B, 0x8C, 0xFF, 0x3D)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(kAccentSoft));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kAccentLine));
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kAccentText));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const bool pressed = FittedButton(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    return pressed;
}

namespace {

// The square ghost button behind IconOnlyButton; returns {clicked, hovered} and its rect.
bool GhostSquare(const char* id, float side, bool on, bool* hovered, ImVec2* a, ImVec2* b)
{
    const float s = side > 0.0f ? side : ControlH();
    const bool  clicked = ImGui::InvisibleButton(id, ImVec2(s, s));
    *hovered = ImGui::IsItemHovered();
    *a = ImGui::GetItemRectMin();
    *b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (on) {
        dl->AddRectFilled(*a, *b, Ink(kAccentSoft), kRadiusMd);
        dl->AddRect(*a, *b, Ink(kAccentLine), kRadiusMd);
    } else if (*hovered || ImGui::IsItemActive()) {
        dl->AddRectFilled(*a, *b, Ink(ImGui::IsItemActive() ? kAccentSoft : kHover), kRadiusMd);
    }
    return clicked;
}

void TipIfHovered(const char* tip)
{
    if (tip && tip[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", tip);
}

}  // namespace

bool IconOnlyButton(const char* id, const char* glyph, const char* tip, float side, bool on)
{
    bool   hovered = false;
    ImVec2 a, b;
    const bool clicked = GhostSquare(id, side, on, &hovered, &a, &b);
    if (glyph && glyph[0]) {
        const ImVec2 ts = ImGui::CalcTextSize(glyph);
        ImGui::GetWindowDrawList()->AddText(ImVec2(std::floor((a.x + b.x - ts.x) * 0.5f), std::floor((a.y + b.y - ts.y) * 0.5f)),
                                            Ink(on ? kAccent : (hovered ? kText : kMuted)), glyph);
    }
    TipIfHovered(tip);
    return clicked;
}

bool IconOnlyButton(const char* id, const Icon& icon, const char* tip, float side, bool on)
{
    bool   hovered = false;
    ImVec2 a, b;
    const bool clicked = GhostSquare(id, side, on, &hovered, &a, &b);
    Icon ic = icon;
    ic.col = on ? kAccent : (hovered ? kText : icon.col);
    const float s = std::min(IconPx(), b.y - a.y - 4.0f);
    PaintIconAt(ImGui::GetWindowDrawList(), ic, std::floor((a.x + b.x - s) * 0.5f), a.y, b.y - a.y, s);
    TipIfHovered(tip);
    return clicked;
}

bool LinkButton(const char* label)
{
    const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    const float  h = ImGui::GetFrameHeight();
    const bool   clicked = ImGui::InvisibleButton(label, ImVec2(std::max(1.0f, ts.x), h));
    const bool   hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImU32  col = Ink(hovered ? kAccentText : kAccentInk);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  ty = a.y + (h - ts.y) * 0.5f;
    dl->AddText(ImVec2(a.x, ty), col, label, LabelEnd(label));
    // text-decoration: underline; text-underline-offset 2 px (under the baseline).
    const float uy = std::floor(ty + ImGui::GetFont()->Ascent + Dp(2.0f)) + 0.5f;
    dl->AddLine(ImVec2(a.x, uy), ImVec2(a.x + ts.x, uy), col, 1.0f);
    return clicked;
}

bool AddButton(const char* label, const ImVec2& size)
{
    const float pad = Dp(10.0f);
    const float nat = TextWidth(label) + 2.0f * pad;
    const float avail = ImGui::GetContentRegionAvail().x;
    float       w = size.x > 0.0f ? size.x : (size.x < 0.0f ? std::max(1.0f, avail + size.x) : nat);
    if (!WindowAutoSizes() && w > avail) w = std::max(ControlH(), avail);
    const float h = size.y > 0.0f ? size.y : ControlH();
    const bool  clicked = ImGui::InvisibleButton(label, ImVec2(std::max(1.0f, w), h));
    const bool  hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    if (hovered) dl->AddRect(a, b, Ink(kStrokeStrong), kRadiusSm);
    else DashedRect(dl, a, b, Ink(kStrokeStrong), kRadiusSm);
    const std::string text(label, LabelEnd(label));
    const float       tw = ImGui::CalcTextSize(text.c_str()).x;
    const float       tx = tw <= w - 2.0f * pad ? a.x + (w - tw) * 0.5f : a.x + Dp(4.0f);
    const bool        cut = EllipsisText(dl, ImVec2(tx, a.y + (h - ImGui::GetTextLineHeight()) * 0.5f), b.x - Dp(4.0f),
                                         Ink(hovered ? kText : kFaint), text);
    if (cut && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", text.c_str());
    return clicked;
}

bool HelpMark(const char* id, const char* tip)
{
    const float s = Dp(18.0f);
    const float h = std::max(s, ImGui::GetFrameHeight());
    const bool  clicked = ImGui::InvisibleButton(id, ImVec2(s, h));
    const bool  hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 c(a.x + s * 0.5f, a.y + h * 0.5f);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    if (hovered) dl->AddCircleFilled(c, s * 0.5f - 0.5f, Ink(kHover), 24);
    dl->AddCircle(c, s * 0.5f - 0.5f, Ink(kStrokeStrong), 24, 1.0f);
    ImFont*      f = FontSmall();
    const ImVec2 ts = TextSizeIn(f, "?");
    AddTextIn(dl, f, ImVec2(std::floor(c.x - ts.x * 0.5f), std::floor(c.y - ts.y * 0.5f)), Ink(hovered ? kText : kMuted), "?");
    TipIfHovered(tip);
    return clicked;
}

// ---- tags (13-7 B3) ----------------------------------------------------------------------

namespace {

struct TagColours {
    ImU32 text, edge, fill;
};
TagColours ToneColours(TagTone tone)
{
    switch (tone) {
    case TagTone::Project: return {kAccentInk, kAccentLine, kBg};
    case TagTone::User: return {kUserInk, kUserLine, kBg};
    case TagTone::Bad: return {kBadInk, kBadLine, kBadSoft};
    case TagTone::Warn: return {kWarn, kWarnLine, kBg};
    case TagTone::Ok: return {kOk, kOkLine, kBg};
    default: return {kMuted, kStroke, kBg};
    }
}

float PillH() { return std::floor(FontSmall()->FontSize + Dp(3.0f)); }  // .tag: 11 px line + pad 1 + edge 1, twice

// Puts an item of width w on the current line, ControlH() high (the frame line), and returns the
// y at which a box `box_h` high sits centred on it.
float LineItem(float w, float box_h, ImVec2* p)
{
    *p = ImGui::GetCursorScreenPos();
    const float h = std::max(box_h, ImGui::GetFrameHeight());
    ImGui::Dummy(ImVec2(std::max(1.0f, w), h));
    return std::floor(p->y + (h - box_h) * 0.5f);
}

}  // namespace

float TagHeight() { return PillH(); }
float SmallTagHeight() { return Dp(17.0f); }

float TagWidth(const char* text, bool dot)
{
    return Dp(8.0f) + (dot ? Dp(8.0f) + Dp(5.0f) : 0.0f) + TextSizeIn(FontSmall(), text).x + Dp(8.0f);
}

float DrawTag(ImDrawList* dl, ImVec2 p, const char* text, TagTone tone, ImU32 dot)
{
    const TagColours c = ToneColours(tone);
    const float      w = TagWidth(text, dot != 0), h = PillH();
    const ImVec2     b(p.x + w, p.y + h);
    dl->AddRectFilled(p, b, Ink(c.fill), h * 0.5f);
    dl->AddRect(p, b, Ink(c.edge), h * 0.5f);
    float x = p.x + Dp(8.0f);
    if (dot) {
        dl->AddCircleFilled(ImVec2(x + Dp(4.0f), p.y + h * 0.5f), Dp(4.0f), Ink(dot), 12);
        x += Dp(8.0f) + Dp(5.0f);
    }
    ImFont* f = FontSmall();
    AddTextIn(dl, f, ImVec2(x, std::floor(p.y + (h - f->FontSize) * 0.5f)), Ink(c.text), text);
    return w;
}

void Tag(const char* text, TagTone tone, ImU32 dot)
{
    ImVec2      p;
    const float w = TagWidth(text, dot != 0);
    const float y = LineItem(w, PillH(), &p);
    DrawTag(ImGui::GetWindowDrawList(), ImVec2(p.x, y), text, tone, dot);
}

void StateTag(ImU32 dot, const char* text)
{
    Tag(text, TagTone::Neutral, dot);
}

float SmallTagWidth(const char* text)
{
    return Dp(5.0f) + TextSizeIn(FontTiny(), text).x + Dp(5.0f);
}

float DrawSmallTag(ImDrawList* dl, ImVec2 p, const char* text)
{
    const float  w = SmallTagWidth(text), h = Dp(17.0f);  // .dtag: line-height 15 + its edge
    const ImVec2 b(p.x + w, p.y + h);
    dl->AddRect(p, b, Ink(kStrokeStrong), Dp(4.0f));
    ImFont* f = FontTiny();
    AddTextIn(dl, f, ImVec2(p.x + Dp(5.0f), std::floor(p.y + (h - f->FontSize) * 0.5f)), Ink(kMuted), text);
    return w;
}

void SmallTag(const char* text)
{
    ImVec2      p;
    const float y = LineItem(SmallTagWidth(text), Dp(17.0f), &p);
    DrawSmallTag(ImGui::GetWindowDrawList(), ImVec2(p.x, y), text);
}

float CountChipWidth(const char* text)
{
    return Dp(8.0f) + TextSizeIn(FontSmall(), text).x + Dp(8.0f);
}

void CountChip(const char* text)
{
    ImVec2      p;
    const float w = CountChipWidth(text), h = PillH();
    const float y = LineItem(w, h, &p);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), Ink(kAccentSoft), h * 0.5f);
    dl->AddRect(ImVec2(p.x, y), ImVec2(p.x + w, y + h), Ink(kAccentLine), h * 0.5f);
    ImFont* f = FontSmall();
    AddTextIn(dl, f, ImVec2(p.x + Dp(8.0f), std::floor(y + (h - f->FontSize) * 0.5f)), Ink(kAccentText), text);
}

void Caption(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

namespace {

// `text` in UPPERCASE with the mockup's letter-spacing (.06em), drawn from p in `font`.
// Returns its width.
float SpacedCaps(ImDrawList* dl, ImFont* font, ImVec2 p, ImU32 col, const char* text, bool draw)
{
    const float spacing = font->FontSize / 1.33f * 0.06f;  // .06 em of the CSS size
    float       x = p.x;
    const char* s = text;
    const char* end = LabelEnd(text);
    while (s < end && *s) {
        unsigned int cp = 0;
        const int    n = ImTextCharFromUtf8(&cp, s, end);
        if (n <= 0) break;
        if (cp < 0x80) cp = static_cast<unsigned int>(std::toupper(static_cast<int>(cp)));
        char buf[5] = {};
        ImTextCharToUtf8(buf, cp);
        if (draw) dl->AddText(font, font->FontSize, ImVec2(x, p.y), col, buf);
        x += font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, buf).x + spacing;
        s += n;
    }
    return std::max(0.0f, x - p.x - spacing);
}

}  // namespace

void GroupCaption(const char* text, ImU32 col)
{
    ImFont*      f = FontSmall();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = SpacedCaps(dl, f, p, 0, text, false);
    SpacedCaps(dl, f, p, Ink(col ? col : kFaint), text, true);
    ImGui::Dummy(ImVec2(std::max(1.0f, w), f->FontSize));
}

void SubText(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

namespace {

// One DragNumber at a time is dragged or typed into.
struct DragNumberState {
    ImGuiID     drag = 0;          // the field being dragged
    DragGesture g;                 // its gesture (ui_drag_number.h)
    ImGuiID     typing = 0;        // the field being typed into
    int         focus_frames = 0;  // frames left for the text field to take the focus
    char        buf[64] = {};
    int         seen_frame = -1;   // the frame the dragged / typed field was last drawn
};
DragNumberState g_dn;

double RoundTo(double v, int decimals) { return DragRound(v, decimals); }

bool ParseTyped(const char* text, double* out)
{
    std::string s;
    for (const char* p = text; *p; ++p) {
        if (*p == ' ') continue;
        s += (*p == ',') ? '.' : *p;  // a decimal comma reads as a point
    }
    if (!s.empty() && s[0] == '+') s.erase(0, 1);
    if (s.empty()) return false;
    double v = 0.0;
    const std::from_chars_result r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

void FormatNumber(char* out, size_t n, double v, int decimals, bool trim)
{
    if (trim) std::snprintf(out, n, "%s", flow::TrimmedNumber(RoundTo(v, decimals), decimals).c_str());
    else std::snprintf(out, n, "%.*f", decimals < 0 ? 0 : decimals, RoundTo(v, decimals));
}

}  // namespace

bool DragNumberActive()
{
    // A field no longer drawn (its rule went away) holds nothing.
    if (g_dn.seen_frame < ImGui::GetFrameCount() - 1) {
        g_dn.drag = 0;
        g_dn.typing = 0;
    }
    return g_dn.drag != 0 || g_dn.typing != 0;
}

void DragNumberReset()
{
    g_dn.drag = 0;
    g_dn.typing = 0;
    g_dn.g.cancelled = false;
}

DragNumberEvent DragNumber(const char* id, double* value, double step, int decimals, const char* unit, float width,
                           bool trim)
{
    DragNumberEvent ev = DragNumberEvent::None;
    if (!value) return ev;
    ImGui::PushID(id);
    const ImGuiID gid = ImGui::GetID("##dragnum");
    const ImGuiIO& io = ImGui::GetIO();
    const float h = ImGui::GetFrameHeight();
    ImFont*     mono = FontMonoSmall();
    float       w = width > 0.0f ? width : TextSizeIn(mono, "-0000.00").x + Dp(12.0f);
    // The field gives way to its unit: both end within the room left (never under 24 px).
    if (!WindowAutoSizes())
        w = flow::FieldBeforeUnit(ImGui::GetContentRegionAvail().x, w, UnitWidth(unit), 0.0f, std::min(w, 24.0f));
    char text[64];
    FormatNumber(text, sizeof(text), *value, decimals, trim);

    if (g_dn.typing == gid || g_dn.drag == gid) g_dn.seen_frame = ImGui::GetFrameCount();
    if (g_dn.typing == gid) {
        // Typing: the same raised field, its edge accent-line (the focus ring), the number in mono.
        ImGui::PushFont(mono);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col(kRaised));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Col(kRaised));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Col(kRaised));
        ImGui::PushStyleColor(ImGuiCol_Border, Col(kAccentLine));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusSm);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(5.0f), std::max(0.0f, (h - mono->FontSize) * 0.5f)));
        ImGui::SetNextItemWidth(w);
        if (g_dn.focus_frames > 0) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##type", g_dn.buf, sizeof(g_dn.buf),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool active = ImGui::IsItemActive();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(4);
        ImGui::PopFont();
        if (g_dn.focus_frames > 0) {
            g_dn.focus_frames = active ? 0 : g_dn.focus_frames - 1;
            if (g_dn.focus_frames == 0 && !active) g_dn.typing = 0;  // never got the focus
        } else if (enter || !active) {
            g_dn.typing = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                ev = DragNumberEvent::Cancel;
            } else {
                double v = 0.0;
                if (ParseTyped(g_dn.buf, &v)) {
                    *value = RoundTo(v, decimals);
                    ev = DragNumberEvent::Commit;
                }
            }
        }
    } else {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##drag", ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        if (ImGui::IsItemActivated()) {
            g_dn.drag = gid;
            g_dn.seen_frame = ImGui::GetFrameCount();
        }
        if (g_dn.drag == gid) {
            DragFrame f;
            f.activated = ImGui::IsItemActivated();
            f.active = active;
            f.deactivated = ImGui::IsItemDeactivated();
            f.esc = active && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            f.shift = io.KeyShift;
            f.mouse_x = io.MousePos.x;
            f.mouse_dx = io.MouseDelta.x;
            f.threshold = io.MouseDragThreshold;
            switch (DragStep(g_dn.g, f, step, decimals, value)) {
            case DragStepEvent::Live: ev = DragNumberEvent::Live; break;
            case DragStepEvent::Commit: ev = DragNumberEvent::Commit; break;
            case DragStepEvent::Cancel: ev = DragNumberEvent::Cancel; break;
            case DragStepEvent::Click:  // a click: type a value
                g_dn.typing = gid;
                g_dn.focus_frames = 3;
                g_dn.seen_frame = ImGui::GetFrameCount();
                std::snprintf(g_dn.buf, sizeof(g_dn.buf), "%s", text);
                break;
            default: break;
            }
            if (f.deactivated) g_dn.drag = 0;
        }
        if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        // 13-7 (B6): the .lnum field: raised, a stroke edge (stroke-strong under the mouse), r5,
        // the number in mono 11 right-aligned 5 px from its edge.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p1(p0.x + w, p0.y + h);
        dl->AddRectFilled(p0, p1, Ink(kRaised), kRadiusSm);
        dl->AddRect(p0, p1, Ink((hovered || active) ? kStrokeStrong : kStroke), kRadiusSm);
        FormatNumber(text, sizeof(text), *value, decimals, trim);
        const ImVec2 ts = TextSizeIn(mono, text);
        dl->PushClipRect(p0, p1, true);
        AddTextIn(dl, mono, ImVec2(std::max(p0.x + Dp(3.0f), p1.x - Dp(5.0f) - ts.x), std::floor(p0.y + (h - ts.y) * 0.5f)),
                  Ink(kText), text);
        dl->PopClipRect();
    }
    if (unit && unit[0]) {
        // The unit (.u): 10 px faint, 6 px after the field, centred on it.
        ImGui::SameLine(0.0f, Dp(6.0f));
        ImFont*      tiny = FontTiny();
        const ImVec2 us = TextSizeIn(tiny, unit);
        const ImVec2 up = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(std::max(1.0f, us.x), h));
        AddTextIn(ImGui::GetWindowDrawList(), tiny, ImVec2(up.x, std::floor(up.y + (h - us.y) * 0.5f)), Ink(kFaint), unit);
    }
    ImGui::PopID();
    return ev;
}

// ---- fader (13-7 B5) ---------------------------------------------------------------------

bool Fader(const char* id, float* v, float v_min, float v_max, float width)
{
    if (!v) return false;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool    has_next_w = (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth) != 0;
    const float   avail = ImGui::GetContentRegionAvail().x;
    float         w = width > 0.0f ? width : (has_next_w ? ImGui::CalcItemWidth() : avail);
    if (!WindowAutoSizes()) w = std::min(w, avail);
    w = std::max(w, Dp(20.0f));
    const float h = ControlH();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool   hovered = ImGui::IsItemHovered();
    const bool   active = ImGui::IsItemActive();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float  r = Dp(8.0f);  // the 16 px thumb
    bool         changed = false;
    if (active) {
        const float nv = flow::FaderValueAt(ImGui::GetIO().MousePos.x, a.x, b.x, r, v_min, v_max);
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = std::floor((a.y + b.y) * 0.5f) + 0.5f;
    const float th = Dp(6.0f);  // the track
    const float tx = flow::FaderThumbX(*v, a.x, b.x, r, v_min, v_max);
    dl->AddRectFilled(ImVec2(a.x, cy - th * 0.5f), ImVec2(b.x, cy + th * 0.5f), Ink(kStrokeStrong), th * 0.5f);
    if (tx > a.x) dl->AddRectFilled(ImVec2(a.x, cy - th * 0.5f), ImVec2(tx, cy + th * 0.5f), Ink(kAccent), th * 0.5f);
    dl->AddCircleFilled(ImVec2(tx, cy), r, Ink((hovered || active) ? kPrimaryHover : kAccent), 24);
    return changed;
}

// ---- text fields (13-7 B7) ---------------------------------------------------------------

namespace {

float FieldWidth(float width)
{
    const float avail = ImGui::GetContentRegionAvail().x;
    if (WindowAutoSizes()) return width > 0.0f ? width : Dp(150.0f);
    return std::max(Dp(24.0f), width > 0.0f ? std::min(width, avail) : avail);
}

// The state of an InputText before it is drawn (its look depends on it): last frame's hover, now's focus.
void FieldState(const char* id, bool* hovered, bool* active)
{
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiID       iid = ImGui::GetID(id);
    *hovered = g.HoveredIdPreviousFrame == iid || g.HoveredId == iid;
    *active = g.ActiveId == iid;
}

}  // namespace

bool TextField(const char* id, const char* hint, char* buf, size_t buf_size, float width, int flags)
{
    const float w = FieldWidth(width);
    const float line = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col(kBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Col(kBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Col(kBg));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Col(kFaint));  // the hint
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(6.0f), std::max(0.0f, (FieldH() - line) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusSm);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(w);
    const bool r = hint ? ImGui::InputTextWithHint(id, hint, buf, buf_size, flags) : ImGui::InputText(id, buf, buf_size, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                        Ink(active ? kAccentLine : (hovered ? kStrokeStrong : kStroke)), kRadiusSm);
    return r;
}

bool TitleField(const char* id, char* buf, size_t buf_size, float width, int flags)
{
    bool hovered = false, active = false;
    FieldState(id, &hovered, &active);
    const float w = FieldWidth(width);
    ImFont*     f = FontHeading();
    ImGui::PushFont(f);
    const ImU32 fill = (hovered || active) ? kBg : IM_COL32(0, 0, 0, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col(fill));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Col(fill));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Col(fill));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(6.0f), std::max(0.0f, (Dp(28.0f) - f->FontSize) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusSm);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(w);
    const bool r = ImGui::InputText(id, buf, buf_size, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Ink(kStrokeStrong), kRadiusSm);
    return r;
}

bool RenameField(const char* id, char* buf, size_t buf_size, float width, int flags)
{
    const float w = FieldWidth(width);
    ImFont*     f = FontTitle();
    ImGui::PushFont(f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col(kBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Col(kBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Col(kBg));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(6.0f), std::max(0.0f, (ControlH() - f->FontSize) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusSm);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(w);
    const bool r = ImGui::InputText(id, buf, buf_size, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Ink(kAccentLine), kRadiusSm);
    return r;
}

bool AddField(const char* id, const char* hint, char* buf, size_t buf_size, float width, int flags)
{
    bool hovered = false, active = false;
    FieldState(id, &hovered, &active);
    const float w = FieldWidth(width);
    const float line = ImGui::GetFontSize();
    const ImU32 fill = active ? kBg : IM_COL32(0, 0, 0, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col(fill));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Col(fill));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Col(fill));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Col(kFaint));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(8.0f), std::max(0.0f, (ControlH() - line) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusSm);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(w);
    const bool r = ImGui::InputTextWithHint(id, hint ? hint : "", buf, buf_size, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (ImGui::IsItemActive() || ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddRect(a, b, Ink(kStrokeStrong), kRadiusSm);
    else DashedRect(ImGui::GetWindowDrawList(), a, b, Ink(kStrokeStrong), kRadiusSm);
    return r;
}

// ---- menus, combos (13-7 B8 / B16) -------------------------------------------------------

namespace {

// The popup window's look (read when it begins): its edge, faint hints / disabled rows /
// captions, pad 4 around the rows with the text 12 px in (a row's highlight is clipped at half
// the padding). Pushes 2 colours and 1 var.
void PushMenuWindowStyle()
{
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStrokeStrong));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Col(kFaint));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(4.0f)));
}
// The rows inside it: 26 px each (the line plus ItemSpacing.y, which a Selectable / MenuItem
// highlight spans). Pushes 1 var.
void PushMenuItemStyle()
{
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Dp(8.0f), std::max(Dp(4.0f), MenuRowH() - ImGui::GetTextLineHeight())));
}
void PushMenuStyle()
{
    PushMenuWindowStyle();
    PushMenuItemStyle();
}
void PopMenuStyle()
{
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

}  // namespace

bool BeginMenuPopup(const char* id, int window_flags)
{
    PushMenuStyle();
    ImGui::SetNextWindowSizeConstraints(ImVec2(Dp(220.0f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopup(id, window_flags)) {
        PopMenuStyle();
        return false;
    }
    return true;
}

bool BeginMenuPopupContextItem(const char* id, int popup_flags)
{
    PushMenuStyle();
    ImGui::SetNextWindowSizeConstraints(ImVec2(Dp(220.0f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopupContextItem(id, popup_flags)) {
        PopMenuStyle();
        return false;
    }
    return true;
}

void EndMenuPopup()
{
    ImGui::EndPopup();
    PopMenuStyle();
}

void MenuCaption(const char* text)
{
    // .pop .mh: 10 px faint UPPERCASE, pad 4 10 2 (the row text's inset).
    ImFont*      f = FontTiny();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = SpacedCaps(dl, f, p, 0, text, false);
    SpacedCaps(dl, f, ImVec2(p.x, p.y + Dp(2.0f)), Ink(kFaint), text, true);
    ImGui::Dummy(ImVec2(std::max(1.0f, w), f->FontSize + Dp(2.0f)));
}

bool BeginComboStyled(const char* id, const char* preview, float width, int flags)
{
    const float chev = Dp(16.0f);  // the chevron's room at the right
    const float nat = std::max(Dp(150.0f), (preview ? ImGui::CalcTextSize(preview).x : 0.0f) + Dp(12.0f) + chev);
    float       w = width > 0.0f ? width : nat;
    if (!WindowAutoSizes()) w = std::min(w, ImGui::GetContentRegionAvail().x);
    w = std::max(w, FieldH());
    ImDrawList*  dl = ImGui::GetWindowDrawList();  // the frame's window (the list opens in its own)
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  line = ImGui::GetFontSize();
    // The list's popup window style now (read when it begins); its rows' spacing only once it is
    // open, so the frame itself is laid out with the caller's spacing.
    PushMenuWindowStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(6.0f), std::max(0.0f, (FieldH() - line) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRadiusSm);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);  // Border = stroke-strong (PushMenuWindowStyle)
    ImGui::SetNextItemWidth(w);
    const bool open = ImGui::BeginCombo(id, nullptr, flags | ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleVar(3);
    if (open) PushMenuItemStyle();
    // The preview, ellipsized before the chevron (a "v" in the text colour).
    const ImVec2 p1(p0.x + w, p0.y + FieldH());
    if (preview && preview[0]) {
        const std::string s(preview);
        EllipsisText(dl, ImVec2(p0.x + Dp(6.0f), p0.y + (FieldH() - ImGui::GetTextLineHeight()) * 0.5f), p1.x - chev - Dp(2.0f),
                     Ink(kText), s);
    }
    const ImVec2 c(p1.x - Dp(11.0f), (p0.y + p1.y) * 0.5f);
    const float  hw = Dp(4.0f), hh = Dp(2.0f);
    const ImVec2 pts[3] = {ImVec2(c.x - hw, c.y - hh), ImVec2(c.x, c.y + hh), ImVec2(c.x + hw, c.y - hh)};
    dl->AddPolyline(pts, 3, Ink(kText), ImDrawFlags_None, Dp(1.3f));
    if (!open) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    return open;
}

void EndComboStyled()
{
    ImGui::EndCombo();
    PopMenuStyle();
}

// ---- checkbox (13-7 B9) ------------------------------------------------------------------

bool Checkbox(const char* label, bool* v)
{
    if (!v) return false;
    const float box = Dp(14.0f), gap = Dp(6.0f);
    const float fh = ImGui::GetFrameHeight();
    const float avail = WindowAutoSizes() ? 1e9f : std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float wrap = std::max(Dp(20.0f), avail - box - gap);
    const char* end = LabelEnd(label);
    const bool  has_text = end > label;
    const ImVec2 ts = has_text ? ImGui::CalcTextSize(label, end, false, wrap) : ImVec2(0.0f, 0.0f);
    const float  h = std::max(fh, ts.y + 2.0f * ImGui::GetStyle().FramePadding.y);
    const bool   clicked = ImGui::InvisibleButton(label, ImVec2(box + (has_text ? gap + ts.x : 0.0f), h));
    if (clicked) *v = !*v;
    const bool   hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  by = std::floor(a.y + (fh - box) * 0.5f);  // centred on the first line
    const ImVec2 b0(a.x, by), b1(a.x + box, by + box);
    const float  r = Dp(3.0f);
    if (*v) {
        dl->AddRectFilled(b0, b1, Ink(hovered ? kPrimaryHover : kAccent), r);
        const float  k = box / 14.0f;
        const ImVec2 tick[3] = {ImVec2(b0.x + 3.2f * k, b0.y + 7.2f * k), ImVec2(b0.x + 5.9f * k, b0.y + 9.9f * k),
                                ImVec2(b0.x + 10.8f * k, b0.y + 4.2f * k)};
        dl->AddPolyline(tick, 3, Ink(kWhite), ImDrawFlags_None, std::max(1.5f, 1.8f * k));
    } else {
        dl->AddRectFilled(b0, b1, Ink(kBg), r);
        dl->AddRect(b0, b1, Ink(hovered ? kFrameHover : kStrokeStrong), r);
    }
    if (has_text)
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + box + gap, a.y + (fh - ImGui::GetTextLineHeight()) * 0.5f),
                    Ink(kText), label, end, wrap);
    return clicked;
}

// ---- rows (13-7 B10) ---------------------------------------------------------------------

namespace {

// A 2 px bar on the left of a rounded row, following its corners.
void LeftBar(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float radius)
{
    dl->PushClipRect(a, ImVec2(a.x + Dp(2.0f), b.y), true);
    dl->AddRectFilled(a, b, col, radius);
    dl->PopClipRect();
}

}  // namespace

void RowBackground(ImDrawList* dl, ImVec2 a, ImVec2 b, unsigned bits, float radius)
{
    if (!dl) return;
    const bool sel = (bits & kRowSelected) != 0, lit = (bits & kRowLit) != 0;
    if (bits & kRowEven) dl->AddRectFilled(a, b, Ink(kRowAlt), radius);
    if (sel || lit) {
        dl->AddRectFilled(a, b, Ink(kAccentSoft), radius);
    } else if (bits & kRowHovered) {
        dl->AddRectFilled(a, b, Ink(kRaised), radius);
    }
    if (sel && !(bits & kRowBarSelect)) dl->AddRect(a, b, Ink(kAccentLine), radius);
    if ((sel && (bits & kRowBarSelect)) || lit) LeftBar(dl, a, b, Ink(kAccent), radius);
    else if (bits & kRowCurrent) LeftBar(dl, a, b, Ink(kAccentLine), radius);
}

bool SelectRow(const char* label, bool selected, unsigned bits, float height, int sflags, float radius)
{
    const float h = height > 0.0f ? height : RowH();
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Header, Col(IM_COL32(0, 0, 0, 0)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Col(IM_COL32(0, 0, 0, 0)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, Col(IM_COL32(0, 0, 0, 0)));
    ImGui::PushStyleColor(ImGuiCol_Text, Col(IM_COL32(0, 0, 0, 0)));
    const bool clicked = ImGui::Selectable(label, selected, sflags | ImGuiSelectableFlags_NoPadWithHalfSpacing, ImVec2(0.0f, h));
    ImGui::PopStyleColor(4);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    // The whole row's hover (its overlaid buttons included), so it does not flicker under them.
    g_selrow_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(a, b);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    RowBackground(dl, a, b, bits | (selected ? kRowSelected : 0u) | (g_selrow_hovered ? kRowHovered : 0u), radius);
    const char* end = LabelEnd(label);
    if (end > label) {
        const std::string text(label, end);
        if (EllipsisText(dl, ImVec2(a.x + Dp(6.0f), a.y + (h - ImGui::GetTextLineHeight()) * 0.5f), b.x - Dp(6.0f), ink, text) &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", text.c_str());
    }
    return clicked;
}

bool SelectRowHovered() { return g_selrow_hovered; }

// ---- chevrons (13-7 B11) -----------------------------------------------------------------

void DrawFoldTriangle(ImDrawList* dl, ImVec2 c, bool open, ImU32 col)
{
    // The mockup's .tri: 8 x 5 pointing down when open, 5 x 8 pointing right when folded.
    const float l = Dp(4.0f), s = Dp(2.5f);
    if (open)
        dl->AddTriangleFilled(ImVec2(c.x - l, c.y - s), ImVec2(c.x + l, c.y - s), ImVec2(c.x, c.y + s), col);
    else
        dl->AddTriangleFilled(ImVec2(c.x - s, c.y - l), ImVec2(c.x - s, c.y + l), ImVec2(c.x + s, c.y), col);
}

bool Chevron(const char* id, bool open, float side)
{
    const float s = side > 0.0f ? side : Dp(18.0f);
    const bool  clicked = ImGui::InvisibleButton(id, ImVec2(s, s));
    const bool  hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    if (hovered) dl->AddRectFilled(a, b, Ink(kHover), kRadiusSm);
    DrawFoldTriangle(dl, ImVec2(std::floor((a.x + b.x) * 0.5f) + 0.5f, std::floor((a.y + b.y) * 0.5f) + 0.5f), open,
                     Ink(hovered ? kText : kMuted));
    return clicked;
}

// ---- dashed outline (13-7 B18) -----------------------------------------------------------

void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float radius, float dash, float gap, float thickness)
{
    if (!dl || b.x <= a.x || b.y <= a.y) return;
    dash = std::max(1.0f, Dp(dash));
    gap = std::max(1.0f, Dp(gap));
    // The rounded rectangle's outline as a polyline (ImGui's own path), then walked in dashes.
    dl->PathRect(ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f), radius);
    ImVector<ImVec2> pts = dl->_Path;
    dl->PathClear();
    if (pts.Size < 2) return;
    pts.push_back(pts[0]);
    bool  on = true;
    float left = dash;
    for (int i = 0; i + 1 < pts.Size; ++i) {
        const ImVec2 p = pts[i], q = pts[i + 1];
        const float  dx = q.x - p.x, dy = q.y - p.y;
        const float  len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0f) continue;
        float t = 0.0f;
        while (t < len) {
            const float stp = std::min(left, len - t);
            if (on)
                dl->AddLine(ImVec2(p.x + dx * t / len, p.y + dy * t / len), ImVec2(p.x + dx * (t + stp) / len, p.y + dy * (t + stp) / len),
                            col, thickness);
            t += stp;
            left -= stp;
            if (left <= 0.0f) {
                on = !on;
                left = on ? dash : gap;
            }
        }
    }
}

// ---- toast (13-7 B15) --------------------------------------------------------------------

bool BeginToast(const char* id, ImVec2 pos, ImVec2 pivot)
{
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStrokeStrong));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kRadiusMd);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(14.0f), Dp(8.0f)));
    ImGui::PushFont(FontTitle());
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    return ImGui::Begin(id, nullptr, kFlags);
}

void EndToast()
{
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void ToastNote(const char* text)
{
    // .toast .u: 11 px faint, 8 px after the line's last item, on its baseline.
    const ImVec2 last_min = ImGui::GetItemRectMin(), last_max = ImGui::GetItemRectMax();
    ImGui::SameLine(0.0f, Dp(8.0f));
    ImFont*      f = FontSmall();
    const ImVec2 ts = TextSizeIn(f, text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  h = std::max(ts.y, last_max.y - last_min.y);
    ImGui::Dummy(ImVec2(std::max(1.0f, ts.x), h));
    const float base = last_min.y + (h - ImGui::GetTextLineHeight()) * 0.5f + ImGui::GetFont()->Ascent;  // the line's baseline
    AddTextIn(ImGui::GetWindowDrawList(), f, ImVec2(p.x, std::floor(base - f->Ascent)), Ink(kFaint), text);
}

// ---- splitter (13-7 B13) -----------------------------------------------------------------

SplitterState SplitterBar(const char* id, ImVec2 size, bool vertical)
{
    SplitterState st;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)));
    st.hovered = ImGui::IsItemHovered();
    st.active = ImGui::IsItemActive();
    st.released = ImGui::IsItemDeactivated();
    st.double_clicked = st.hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (st.active) st.delta = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
    if (st.hovered || st.active) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (st.hovered || st.active) {
        const ImU32 col = Ink(st.active ? kAccent : kAccentLine);
        const float bw = Dp(2.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (vertical) {
            const float x = std::floor(p.x + (size.x - bw) * 0.5f);
            dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + bw, p.y + size.y), col, bw * 0.5f);
        } else {
            const float y = std::floor(p.y + (size.y - bw) * 0.5f);
            dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + size.x, y + bw), col, bw * 0.5f);
        }
    }
    return st;
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

// ---- compartments and aligned sentences ------------------------------------------------------

bool EllipsisText(ImDrawList* dl, ImVec2 p, float max_x, ImU32 col, const std::string& text)
{
    const flow::MeasureFn measure = [](const char* b, const char* e) { return ImGui::CalcTextSize(b, e).x; };
    const size_t keep = flow::EllipsisKeep(text, max_x - p.x, ImGui::CalcTextSize("...").x, measure);
    if (keep >= text.size()) {
        dl->AddText(p, col, text.c_str());
        return false;
    }
    const std::string s = text.substr(0, keep) + "...";
    dl->PushClipRect(ImVec2(p.x, p.y - 1.0f), ImVec2(std::max(p.x, max_x), p.y + ImGui::GetFontSize() + 1.0f), true);
    dl->AddText(p, col, s.c_str());
    dl->PopClipRect();
    return true;
}

void TrackFill(ImDrawList* dl, float x0, float lx, float x1, float y0, float y1, ImU32 lane_fill)
{
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(lx, y1), kRaised, kRadiusSm, ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(lx, y0), ImVec2(x1, y1), lane_fill, kRadiusSm, ImDrawFlags_RoundCornersRight);
}

void TrackEdge(ImDrawList* dl, float x0, float lx, float x1, float y0, float y1, ImU32 edge)
{
    dl->AddLine(ImVec2(lx + 0.5f, y0 + 1.0f), ImVec2(lx + 0.5f, y1 - 1.0f), kStrokeStrong, 1.0f);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), edge, kRadiusSm);
}

void RuleStripe(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float radius)
{
    dl->PushClipRect(p0, ImVec2(p0.x + 3.0f, p1.y), true);
    dl->AddRectFilled(p0, p1, col, radius);
    dl->PopClipRect();
}

void BeginCard(Card& k, float top_pad)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    k.p0 = ImGui::GetCursorScreenPos();
    k.w = ImGui::GetContentRegionAvail().x;
    k.split.Split(dl, 2);
    k.split.SetCurrentChannel(dl, 1);
    ImGui::SetCursorScreenPos(ImVec2(k.p0.x + kCardPad, k.p0.y + (top_pad >= 0.0f ? top_pad : kCardPad)));
    ImGui::BeginGroup();
    // The content ends at the card's inner edge, not at the window's: everything inside measures
    // the room it really has (13-7a: rows ran into the card's right edge and were clipped).
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    k.saved_work_max = win->WorkRect.Max.x;
    k.saved_content_max = win->ContentRegionRect.Max.x;
    const float inner = std::max(k.p0.x + kCardPad + 1.0f, k.p0.x + k.w - kCardPad);
    win->WorkRect.Max.x = std::min(win->WorkRect.Max.x, inner);
    win->ContentRegionRect.Max.x = std::min(win->ContentRegionRect.Max.x, inner);
}

void EndCard(Card& k, ImU32 fill, ImU32 edge)
{
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    win->WorkRect.Max.x = k.saved_work_max;
    win->ContentRegionRect.Max.x = k.saved_content_max;
    ImGui::EndGroup();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bottom = ImGui::GetItemRectMax().y + kCardPad;
    k.split.SetCurrentChannel(dl, 0);
    // 13-7 (A3): the mockup's cards (.secx, .pane) are r-lg.
    dl->AddRectFilled(k.p0, ImVec2(k.p0.x + k.w, bottom), fill, kRadiusLg);
    dl->AddRect(k.p0, ImVec2(k.p0.x + k.w, bottom), edge, kRadiusLg);
    k.split.Merge(dl);
    ImGui::SetCursorScreenPos(ImVec2(k.p0.x, bottom));
    ImGui::Dummy(ImVec2(k.w, std::max(0.0f, kCardGap - ImGui::GetStyle().ItemSpacing.y)));
}

void PushPanelStyle()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(kSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStroke));
}

namespace {

// Puts the cursor at `target` (a line start) through an empty item, so ImGui sees the layout
// reach it (no SetCursorPos-only extent) whatever the item spacing.
void MoveCursorTo(ImVec2 target)
{
    ImGui::SetCursorScreenPos(ImVec2(target.x, target.y - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

}  // namespace

bool BeginSection(Section& s, const char* title, ImTextureID icon, bool foldable)
{
    SectionOpts o;
    o.icon = icon;
    o.foldable = foldable;
    return BeginSection(s, title, o);
}

bool BeginSection(Section& s, const char* title, const SectionOpts& o)
{
    ImGui::PushID(title);
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID("##open");
    bool open = !o.foldable || store->GetBool(open_id, true);
    s.nested = g_section_depth > 0;
    ++g_section_depth;
    s.right_w = std::max(0.0f, o.right_w);
    s.in_header = false;

    // .secx: padding 4 8 8.
    BeginCard(s.card, Dp(4.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float  hh = SectionHeaderH();
    const float  w = std::max(1.0f, s.card.w - 2.0f * kCardPad);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    s.head_min = p;
    s.head_max = ImVec2(p.x + w, p.y + hh);

    // The header: a button over the whole row when the section folds (the right slot's widgets,
    // drawn later, take the mouse before it).
    bool hovered = false;
    if (o.foldable) {
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##head", ImVec2(w, hh))) {
            open = !open;
            store->SetBool(open_id, open);
        }
        hovered = ImGui::IsItemHovered();
    } else {
        ImGui::Dummy(ImVec2(w, hh));
    }
    if (hovered)  // .secx-h:hover: kHover, bleeding 4 px each side, r5
        dl->AddRectFilled(ImVec2(p.x - Dp(4.0f), p.y), ImVec2(p.x + w + Dp(4.0f), p.y + hh), Ink(kHover), kRadiusSm);
    float tx = p.x;
    if (o.foldable) {  // the fold triangle (.tri) on the left, muted, 8 px before the title
        DrawFoldTriangle(dl, ImVec2(p.x + Dp(4.0f), std::floor(p.y + hh * 0.5f) + 0.5f), open, Ink(kMuted));
        tx += Dp(8.0f) + Dp(8.0f);
    }
    if (o.icon) {
        const float is = Dp(16.0f);
        dl->AddImage(o.icon, ImVec2(tx, p.y + (hh - is) * 0.5f), ImVec2(tx + is, p.y + (hh + is) * 0.5f), ImVec2(0, 0),
                     ImVec2(1, 1), Ink(IM_COL32(0xD1, 0xD6, 0xE5, 0xFF)));
        tx += is + Dp(8.0f);
    }
    // The title (13 px) ends before the right slot (ellipsized, the full title as a tooltip); the
    // suffix (11 px faint, on the title's baseline) follows it when there is room.
    const float right_end = p.x + w - (s.right_w > 0.0f ? s.right_w + Dp(8.0f) : 0.0f);
    const char* tend = LabelEnd(title);
    const std::string ttl(title, tend);
    ImFont*     tf = FontTitle();
    const float ty = std::floor(p.y + (hh - tf->FontSize) * 0.5f);
    ImGui::PushFont(tf);
    const bool  cut = EllipsisTextTip(dl, ImVec2(tx, ty), right_end, Ink(kText), ttl);
    const float title_w = ImGui::CalcTextSize(ttl.c_str()).x;
    ImGui::PopFont();
    if (o.suffix && o.suffix[0] && !cut) {
        ImFont*     sf = FontSmall();
        const float sx = tx + title_w + Dp(4.0f);
        if (sx + Dp(16.0f) < right_end) {
            ImGui::PushFont(sf);
            EllipsisTextTip(dl, ImVec2(sx, std::floor(ty + tf->Ascent - sf->Ascent)), right_end, Ink(kFaint), o.suffix);
            ImGui::PopFont();
        }
    }
    if (open) {
        // The divider (.secx-b border-top) 5 px under the header, across the card; the settings
        // 8 px under it.
        const float y = std::floor(p.y + hh + Dp(5.0f)) + 0.5f;
        dl->AddLine(ImVec2(s.card.p0.x + 1.0f, y), ImVec2(s.card.p0.x + s.card.w - 1.0f, y), Ink(kStroke), 1.0f);
        MoveCursorTo(ImVec2(p.x, std::floor(y + 0.5f + Dp(8.0f))));
    }
    s.resume = ImGui::GetCursorScreenPos();
    s.open = open;
    return open;
}

void BeginSectionHeaderRight(Section& s)
{
    if (s.in_header) return;
    s.resume = ImGui::GetCursorScreenPos();
    s.in_header = true;
    const float hh = s.head_max.y - s.head_min.y;
    ImGui::SetCursorScreenPos(ImVec2(s.head_max.x - s.right_w, std::floor(s.head_min.y + (hh - ControlH()) * 0.5f)));
}

void EndSectionHeaderRight(Section& s)
{
    if (!s.in_header) return;
    s.in_header = false;
    MoveCursorTo(s.resume);
}

void EndSection(Section& s)
{
    if (s.in_header) EndSectionHeaderRight(s);
    EndCard(s.card, s.nested ? kBg : kSurface, kStroke);  // .secx .secx: a section inside a section is kBg
    g_section_depth = std::max(0, g_section_depth - 1);
    ImGui::PopID();
}

namespace {
float SentenceGap() { return ImGui::GetStyle().ItemSpacing.x; }
}  // namespace

float SentenceColumnWidth(const char* const* labels, int count, const char* badge)
{
    float w = badge ? ImGui::CalcTextSize(badge).x + 8.0f : 0.0f;
    for (int i = 0; labels && i < count; ++i)
        if (labels[i]) w = std::max(w, ImGui::CalcTextSize(labels[i]).x);
    return w;
}

Sentence MakeSentence(float x0, float right, float column_w)
{
    Sentence s;
    s.x0 = x0;
    s.chips_x = x0 + column_w + SentenceGap();
    s.right = std::max(s.chips_x + 20.0f, right);
    return s;
}

void SentenceRow(Sentence& s, const char* label, bool badge)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fh = ImGui::GetFrameHeight();
    const float y = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(s.x0, y));
    ImGui::Dummy(ImVec2(s.chips_x - SentenceGap() - s.x0, fh));
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float  xr = s.chips_x - SentenceGap();
    if (badge) {
        const float bx = xr - ts.x - 8.0f;
        dl->AddRectFilled(ImVec2(bx, y + 3.0f), ImVec2(xr, y + fh - 3.0f), kBg, 4.0f);
        dl->AddRect(ImVec2(bx, y + 3.0f), ImVec2(xr, y + fh - 3.0f), kStroke, 4.0f);
        dl->AddText(ImVec2(bx + 4.0f, y + (fh - ts.y) * 0.5f), kMuted, label);
    } else {
        dl->AddText(ImVec2(xr - ts.x, y + (fh - ts.y) * 0.5f), kMuted, label);
    }
    s.first = true;
}

void SentenceChip(Sentence& s, float w, float spacing)
{
    const float sp = spacing < 0.0f ? SentenceGap() : spacing;
    if (s.first) {
        ImGui::SameLine(0.0f, SentenceGap());
        s.first = false;
    } else if (ImGui::GetItemRectMax().x + sp + w <= s.right) {
        ImGui::SameLine(0.0f, sp);
    } else {
        ImGui::SetCursorScreenPos(ImVec2(s.chips_x, ImGui::GetCursorScreenPos().y));
    }
}

float NumberFieldWidth(float field_w, const char* unit)
{
    return field_w + UnitWidth(unit);
}

bool VerticalTabs(const char* id, const char* const* labels, int count, int* selected, float width)
{
    if (!labels || count <= 0 || !selected) return false;
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetTextLineHeight() + 10.0f;
    const float pad_x = 12.0f;  // the text's inset (the accent bar sits in it)
    float w = width;
    if (w <= 0.0f) {
        for (int i = 0; i < count; ++i)
            w = std::max(w, ImGui::CalcTextSize(labels[i], nullptr, true).x + 2.0f * pad_x);
    }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const ImVec2 a(p0.x, p0.y + static_cast<float>(i) * (h + 2.0f));
        const ImVec2 b(a.x + w, a.y + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##vtab", ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (clicked && *selected != i) {
            *selected = i;
            changed = true;
        }
        const bool on = (*selected == i);
        if (on) {
            dl->AddRectFilled(a, b, kAccentSoft, kRadiusSm);
            dl->AddRectFilled(ImVec2(a.x, a.y + 4.0f), ImVec2(a.x + 3.0f, b.y - 4.0f), kAccent, 1.5f);
        } else if (hovered) {
            dl->AddRectFilled(a, b, kHover, kRadiusSm);
        }
        const ImVec2 ts = ImGui::CalcTextSize(labels[i], nullptr, true);
        const char* end = std::strstr(labels[i], "##");  // the visible part of the label
        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + pad_x, a.y + (h - ts.y) * 0.5f), on ? kText : kMuted, labels[i], end);
        dl->PopClipRect();
    }

    // One item for the whole column, so the layout continues after it.
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, static_cast<float>(count) * (h + 2.0f) - 2.0f));
    ImGui::PopID();
    return changed;
}

}  // namespace ui
}  // namespace rav

#endif  // _WIN32
