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
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

#include <imgui_internal.h>  // a card narrows the window's work rect; next-item width; hovered ids

#include "ui_flow.h"
#include "ui_flow_math.h"
#include "ui_fold_all.h"

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
    // 13-14 (A5): s.DisabledAlpha keeps ImGui's 0.6. The theme's buttons draw the mockup's disabled
    // skin themselves at full opacity (PushDisabledButtonSkin); a global 1.0 would make every stock
    // widget left in a disabled region (a disabled MenuItem, a Selectable) look enabled.
}

namespace {
float   g_scale = 1.0f;
int     g_seg_hovered = -1;
FontSet g_fonts;
int     g_section_depth = 0;   // sections open around the current one (a nested one is kBg)
bool    g_selrow_hovered = false;
// The toolbars open (.tbb.dis), each with the window it was begun in, for the frame they were begun in:
// a popup / card / child window opened inside one is another window, so never a toolbar; a toolbar
// left open (a missed EndToolbar) is forgotten on the next frame.
ImGuiWindow* g_toolbar_win[16] = {};
int          g_toolbar_depth = 0;
int          g_toolbar_frame = -1;
// BeginDisabledLook's pushes, one entry per open look: bit 0 = colours, bit 1 = alpha.
unsigned g_dislook[32] = {};
int      g_dislook_n = 0;

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

// 13-14 (A5): inside a disabled region, the alpha from before ImGui's fade, so a widget drawing its
// own disabled look (faint) is not faded again. Returns whether it pushed (pass to PopCrisp).
bool PushCrisp()
{
    if (!ItemDisabled()) return false;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetCurrentContext()->DisabledAlphaBackup);
    return true;
}
void PopCrisp(bool pushed)
{
    if (pushed) ImGui::PopStyleVar();
}

// True when the current item is a toolbar's (BeginToolbar in this window, this frame).
bool InToolbar()
{
    if (g_toolbar_frame != ImGui::GetFrameCount() || g_toolbar_depth <= 0) return false;
    const int top = std::min(g_toolbar_depth, static_cast<int>(sizeof(g_toolbar_win) / sizeof(g_toolbar_win[0]))) - 1;
    return g_toolbar_win[top] == ImGui::GetCurrentWindowRead();
}

// The theme's own item spacing and frame padding (ApplyRavTheme's, at the screen's DPI), pushed inside a
// popup or card so its content never inherits a toolbar's or a menu's metrics. Pushes 2 vars.
void PushThemeMetrics()
{
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Dp(6.0f), Dp(7.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(8.0f), Dp(3.0f)));
}
void PopThemeMetrics() { ImGui::PopStyleVar(2); }

}  // namespace

// ---- disabled look (13-14, A5 / B2) --------------------------------------------------------

bool ItemDisabled()
{
    const ImGuiContext* g = ImGui::GetCurrentContext();
    return g && (g->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
}

void BeginDisabledLook(bool disabled)
{
    unsigned bits = 0;
    if (disabled) {
        if (ItemDisabled()) {  // already faded by an outer region: back to full opacity
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetCurrentContext()->DisabledAlphaBackup);
            bits |= 2u;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kFaint));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, Col(kFaint));
        bits |= 1u;
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);  // read by BeginDisabled only
        ImGui::BeginDisabled(true);
        ImGui::PopStyleVar();
    } else {
        ImGui::BeginDisabled(false);
    }
    if (g_dislook_n < static_cast<int>(sizeof(g_dislook) / sizeof(g_dislook[0]))) g_dislook[g_dislook_n] = bits;
    ++g_dislook_n;
}

void EndDisabledLook()
{
    if (g_dislook_n <= 0) return;
    --g_dislook_n;
    const unsigned bits =
        g_dislook_n < static_cast<int>(sizeof(g_dislook) / sizeof(g_dislook[0])) ? g_dislook[g_dislook_n] : 0u;
    ImGui::EndDisabled();
    if (bits & 1u) ImGui::PopStyleColor(2);
    if (bits & 2u) ImGui::PopStyleVar();
}

bool PushDisabledButtonSkin()
{
    if (!ItemDisabled()) return false;
    // .btn[disabled]: raised fill, faint text; a ghost button stays transparent, and so does a
    // toolbar button (.tbb.dis). Hovered / active never show (a disabled item is never hovered).
    const bool   ghost = InToolbar() || ImGui::GetStyleColorVec4(ImGuiCol_Button).w <= 0.0f;
    const ImVec4 fill = ghost ? ImVec4(0.0f, 0.0f, 0.0f, 0.0f) : Col(kRaised);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetCurrentContext()->DisabledAlphaBackup);
    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, fill);
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kFaint));
    return true;
}

void PopDisabledButtonSkin(bool pushed)
{
    if (!pushed) return;
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
}

// ---- tooltips (13-14, G-14 / B14) ----------------------------------------------------------

namespace {

// The tooltip window in the theme's look (pad 8 6, stroke-strong edge, the body font). Replaces a
// tooltip already begun this frame, as SetTooltip. When true: EndStyledTooltip.
bool BeginStyledTooltip()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(8.0f), Dp(6.0f)));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStrokeStrong));
    const bool open = ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (open) {
        // The tooltip's own text colours: a disabled region (BeginDisabledLook, a disabled skin) may
        // have pushed kFaint for its items; ImGui already lifts its fade inside a tooltip.
        ImGui::PushFont(DefaultFont());
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kText));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, Col(kMuted));
    }
    return open;
}
void EndStyledTooltip()
{
    ImGui::PopStyleColor(2);
    ImGui::PopFont();
    ImGui::EndTooltip();
}

}  // namespace

void ShowTip(const char* text)
{
    if (!text || !text[0] || !BeginStyledTooltip()) return;
    ImGui::TextUnformatted(text);
    EndStyledTooltip();
}

void ItemTipText(const char* text)
{
    if (!text || !text[0] || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ShowTip(text);
}

void ItemTip(const char* fmt, ...)
{
    if (!fmt || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    char    buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ShowTip(buf);
}

bool BeginItemTip(float wrap_em, int hovered_flags)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | hovered_flags) || !BeginStyledTooltip()) return false;
    ImGui::PushTextWrapPos(wrap_em > 0.0f ? ImGui::GetFontSize() * wrap_em : 0.0f);
    return true;
}

void EndItemTip()
{
    ImGui::PopTextWrapPos();
    EndStyledTooltip();
}

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
               int disabled_index, const Icon* icons, const char* const* badges, bool follow)
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
        if (on && follow) {  // 13-62: followed, not its own: no fill, a dashed edge
            if (hovered) dl->AddRectFilled(a, b, Ink(kHover), kRadiusSm);
            DashedRect(dl, a, b, Ink(kStrokeStrong), kRadiusSm);
        } else if (on) {
            dl->AddRectFilled(a, b, Ink(kRaised), kRadiusSm);
            dl->AddRect(a, b, Ink(kStrokeStrong), kRadiusSm);
        } else if (hovered && !disabled) {
            dl->AddRectFilled(a, b, Ink(kHover), kRadiusSm);
        }
        const char*       end = std::strstr(labels[i], "##");  // the visible part of the label
        const std::string text = end ? std::string(labels[i], end) : std::string(labels[i]);
        const float       room = icons && icons[i].fn ? icon_room : 0.0f;  // the option's icon, before its label
        const float       tw = TextWidth(labels[i]) + room + badge_w[i];
        const ImU32       ink = Ink(disabled ? kFaint : (on && !follow ? kText : (badge_w[i] > 0.0f ? kAccentText : kMuted)));
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
                ShowTip(text.c_str());
        } else if (EllipsisText(dl, ImVec2(a.x + 4.0f, ty), b.x - 4.0f, ink, text) && hovered) {
            ShowTip(text.c_str());
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
    if (!on || ItemDisabled()) return SolidButton(label, size);  // disabled: the disabled skin, no accent
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

bool ConfirmButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, Col(kConfirmAct));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(kConfirmActHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(kConfirmAct));
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kConfirmActText));
    const bool pressed = FittedButton(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

namespace {

// The ghost button behind IconOnlyButton (w x h); returns clicked, *hovered and its rect.
bool GhostRect(const char* id, float w, float h, bool on, bool framed, bool* hovered, ImVec2* a, ImVec2* b)
{
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, w), std::max(1.0f, h)));
    *hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    *a = ImGui::GetItemRectMin();
    *b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (on) {
        dl->AddRectFilled(*a, *b, Ink(kAccentSoft), kRadiusMd);
        dl->AddRect(*a, *b, Ink(kAccentLine), kRadiusMd);
    } else if (framed) {  // .colbtn over the 3D view
        dl->AddRectFilled(*a, *b, Ink(active ? kAccentSoft : (*hovered ? kHover : kOverlayBg)), kRadiusMd);
        dl->AddRect(*a, *b, Ink(kStroke), kRadiusMd);
    } else if (*hovered || active) {
        dl->AddRectFilled(*a, *b, Ink(active ? kAccentSoft : kHover), kRadiusMd);
    }
    return clicked;
}

void TipIfHovered(const char* tip)
{
    if (tip && tip[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ShowTip(tip);
}

// An icon-only button's ink: faint when disabled, accent when on, kText under the mouse.
ImU32 GlyphInk(bool on, bool hovered, ImU32 rest)
{
    return ItemDisabled() ? kFaint : (on ? kAccent : (hovered ? kText : rest));
}

}  // namespace

bool IconOnlyButton(const char* id, const char* glyph, const char* tip, float side, bool on)
{
    bool         hovered = false;
    ImVec2       a, b;
    const float  s = side > 0.0f ? side : ControlH();
    const bool   clicked = GhostRect(id, s, s, on, false, &hovered, &a, &b);
    const bool   crisp = PushCrisp();
    if (glyph && glyph[0]) {
        const ImVec2 ts = ImGui::CalcTextSize(glyph);
        ImGui::GetWindowDrawList()->AddText(ImVec2(std::floor((a.x + b.x - ts.x) * 0.5f), std::floor((a.y + b.y - ts.y) * 0.5f)),
                                            Ink(GlyphInk(on, hovered, kMuted)), glyph);
    }
    PopCrisp(crisp);
    TipIfHovered(tip);
    return clicked;
}

bool IconOnlyButton(const char* id, const Icon& icon, const char* tip, float side, bool on)
{
    IconOnlyOpts o;
    o.w = side;
    o.on = on;
    return IconOnlyButton(id, icon, tip, o);
}

bool IconOnlyButton(const char* id, const Icon& icon, const char* tip, const IconOnlyOpts& o)
{
    bool        hovered = false;
    ImVec2      a, b;
    const float w = o.w > 0.0f ? o.w : ControlH();
    const float h = o.h > 0.0f ? o.h : w;
    const bool  clicked = GhostRect(id, w, h, o.on, o.framed, &hovered, &a, &b);
    const bool  crisp = PushCrisp();
    Icon        ic = icon;
    ic.col = GlyphInk(o.on, hovered, o.tint ? o.tint : icon.col);
    const float s = o.icon_px > 0.0f ? o.icon_px : std::min(IconPx(), b.y - a.y - 4.0f);
    PaintIconAt(ImGui::GetWindowDrawList(), ic, std::floor((a.x + b.x - s) * 0.5f), a.y, b.y - a.y, s);
    PopCrisp(crisp);
    TipIfHovered(tip);
    return clicked;
}

bool IconOnlyButton(const char* id, ImTextureID tex, const char* tip, float side, bool on)
{
    IconOnlyOpts o;
    o.w = side;
    o.on = on;
    return IconOnlyButton(id, tex, tip, o);
}

bool IconOnlyButton(const char* id, ImTextureID tex, const char* tip, const IconOnlyOpts& o)
{
    bool        hovered = false;
    ImVec2      a, b;
    const float w = o.w > 0.0f ? o.w : ControlH();
    const float h = o.h > 0.0f ? o.h : w;
    const bool  clicked = GhostRect(id, w, h, o.on, o.framed, &hovered, &a, &b);
    if (tex) {
        const bool  crisp = PushCrisp();
        const float s = o.icon_px > 0.0f ? o.icon_px : std::max(1.0f, std::min(b.x - a.x, b.y - a.y) - Dp(4.0f));
        const ImVec2 p(std::floor((a.x + b.x - s) * 0.5f), std::floor((a.y + b.y - s) * 0.5f));
        // A lit texture icon reads kText (its accent-soft frame says it is on).
        const ImU32 ink = ItemDisabled() ? kFaint : ((o.on || hovered) ? kText : (o.tint ? o.tint : kMuted));
        ImGui::GetWindowDrawList()->AddImage(tex, p, ImVec2(p.x + s, p.y + s), ImVec2(0, 0), ImVec2(1, 1), Ink(ink));
        PopCrisp(crisp);
    }
    TipIfHovered(tip);
    return clicked;
}

bool LinkButton(const char* label, ImU32 quiet)
{
    const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    const float  h = ImGui::GetFrameHeight();
    const bool   clicked = ImGui::InvisibleButton(label, ImVec2(std::max(1.0f, ts.x), h));
    const bool   hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImVec2 a = ImGui::GetItemRectMin();
    const bool   crisp = PushCrisp();
    ImU32        col = 0;
    if (ItemDisabled()) col = Ink(kFaint);
    else if (quiet) col = Ink(hovered ? kText : quiet);
    else col = Ink(hovered ? kAccentText : kAccentInk);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  ty = a.y + (h - ts.y) * 0.5f;
    dl->AddText(ImVec2(a.x, ty), col, label, LabelEnd(label));
    // text-decoration: underline; text-underline-offset 2 px (under the baseline). A quiet link is
    // underlined under the mouse only.
    if (!quiet || hovered) {
        const float uy = std::floor(ty + ImGui::GetFont()->Ascent + Dp(2.0f)) + 0.5f;
        dl->AddLine(ImVec2(a.x, uy), ImVec2(a.x + ts.x, uy), col, 1.0f);
    }
    PopCrisp(crisp);
    return clicked;
}

namespace {
float KeyCapWidth(const char* key) { return key && key[0] ? ImGui::CalcTextSize(key).x + Dp(8.0f) : 0.0f; }
}  // namespace

float AddButtonWidth(const char* label, const char* key)
{
    const float kw = KeyCapWidth(key);
    return TextWidth(label) + (kw > 0.0f ? Dp(8.0f) + kw : 0.0f) + 2.0f * Dp(10.0f);
}

bool AddButton(const char* label, const ImVec2& size, const char* key, bool pill)
{
    const float pad = Dp(10.0f);
    const float gap = Dp(8.0f);
    const float kw = KeyCapWidth(key);
    const float nat = AddButtonWidth(label, key);
    const float avail = ImGui::GetContentRegionAvail().x;
    float       w = size.x > 0.0f ? size.x : (size.x < 0.0f ? std::max(1.0f, avail + size.x) : nat);
    if (!WindowAutoSizes() && w > avail) w = std::max(ControlH(), avail);
    const std::string text(label, LabelEnd(label));
    const float       tw = ImGui::CalcTextSize(text.c_str()).x;
    w = std::max(w, flow::GlyphButtonMinWidth(text, tw));  // 13-47: a lone "+" keeps its glyph's width
    const float h = size.y > 0.0f ? size.y : ControlH();
    const bool  clicked = ImGui::InvisibleButton(label, ImVec2(std::max(1.0f, w), h));
    const bool  hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const bool   crisp = PushCrisp();
    const float  r = pill ? h * 0.5f : kRadiusSm;
    if (hovered) dl->AddRect(a, b, Ink(kStrokeStrong), r);
    else DashedRect(dl, a, b, Ink(kStrokeStrong), r);
    const ImU32       ink = Ink(ItemDisabled() ? kFaint : (hovered ? kText : kFaint));
    const float       ty = a.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    const float       ky = std::floor(a.y + (h - ImGui::GetTextLineHeight() - Dp(3.0f)) * 0.5f);
    bool              cut = false;
    if (tw + (kw > 0.0f ? gap + kw : 0.0f) <= w - 2.0f * pad) {  // label and key cap centred together
        const float tx = a.x + (w - tw - (kw > 0.0f ? gap + kw : 0.0f)) * 0.5f;
        dl->AddText(ImVec2(tx, ty), ink, text.c_str());
        if (kw > 0.0f) KeyCap(dl, ImVec2(tx + tw + gap, ky), key, ink);
    } else if (kw > 0.0f && w - 2.0f * Dp(4.0f) - gap - kw >= Dp(20.0f)) {  // the key cap stays at the right
        cut = EllipsisText(dl, ImVec2(a.x + Dp(4.0f), ty), b.x - Dp(4.0f) - gap - kw, ink, text);
        KeyCap(dl, ImVec2(b.x - Dp(4.0f) - kw, ky), key, ink);
    } else if (flow::ButtonLabelFit(text, tw, w, Dp(4.0f), pill ? std::max(Dp(4.0f), 0.3f * h) : 1.0f) != flow::LabelFit::Cut) {  // 13-30: whole, centred
        dl->AddText(ImVec2(std::floor(a.x + (w - tw) * 0.5f), ty), ink, text.c_str());
    } else {
        cut = EllipsisText(dl, ImVec2(a.x + Dp(4.0f), ty), b.x - Dp(4.0f), ink, text);
    }
    PopCrisp(crisp);
    if (cut && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ShowTip(text.c_str());
    return clicked;
}

namespace {

// The chip's pill (ChipButton, SplitChip): r = h / 2, kBg / kHover / accent-soft fill and its edge.
void DrawChipPill(ImDrawList* dl, ImVec2 a, ImVec2 b, bool on, bool hovered)
{
    const float r = (b.y - a.y) * 0.5f;
    dl->AddRectFilled(a, b, Ink(on ? kAccentSoft : (hovered ? kHover : kBg)), r);
    dl->AddRect(a, b, Ink(on ? kAccentLine : (hovered ? kStrokeStrong : kStroke)), r);
}

ImU32 ChipInk(bool on, bool hovered, bool dis)
{
    return dis ? kFaint : (on ? kAccentText : (hovered ? kText : kMuted));
}

// The check mark of Checkbox, in a `box` square at b0 (SplitChip's tick slot too).
void DrawTick(ImDrawList* dl, ImVec2 b0, float box, ImU32 col)
{
    const float  k = box / 14.0f;
    const ImVec2 tick[3] = {ImVec2(b0.x + 3.2f * k, b0.y + 7.2f * k), ImVec2(b0.x + 5.9f * k, b0.y + 9.9f * k),
                            ImVec2(b0.x + 10.8f * k, b0.y + 4.2f * k)};
    dl->AddPolyline(tick, 3, col, ImDrawFlags_None, std::max(1.5f, 1.8f * k));
}

}  // namespace

bool ChipButton(const char* label, const ImVec2& size, bool on)
{
    const float pad = Dp(7.0f);
    const float h = size.y > 0.0f ? size.y : ControlH();
    const float nat = TextWidth(label) + 2.0f * pad;
    const float avail = ImGui::GetContentRegionAvail().x;
    float       w = size.x > 0.0f ? size.x : (size.x < 0.0f ? std::max(1.0f, avail + size.x) : nat);
    if (!WindowAutoSizes() && w > avail) w = std::max(h, avail);
    const std::string text(label, LabelEnd(label));
    const float       tw = ImGui::CalcTextSize(text.c_str()).x;
    w = std::max(w, flow::GlyphButtonMinWidth(text, tw));  // 13-30: a lone "+" keeps its glyph's width
    const bool   clicked = ImGui::InvisibleButton(label, ImVec2(std::max(1.0f, w), h));
    const bool   hovered = ImGui::IsItemHovered();
    const bool   dis = ItemDisabled();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const bool   crisp = PushCrisp();
    DrawChipPill(dl, a, b, on, hovered);
    const ImU32       ink = Ink(ChipInk(on, hovered, dis));
    const float       ty = a.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    // 13-30: a label that fits (with or without the padding) or a lone glyph is drawn whole, centred.
    bool cut = false;
    if (flow::ButtonLabelFit(text, tw, w, pad, std::min(pad, 0.3f * h)) != flow::LabelFit::Cut)
        dl->AddText(ImVec2(std::floor(a.x + (w - tw) * 0.5f), ty), ink, text.c_str());
    else
        cut = EllipsisText(dl, ImVec2(a.x + pad, ty), b.x - pad, ink, text);
    PopCrisp(crisp);
    if (cut && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ShowTip(text.c_str());
    return clicked;
}

// Story 10-10e: the two-zone chip. Metrics: 6 px left pad, an 11 px tick slot, 4 px, the label,
// 6 px, a 1 px divider, then the ⚙ zone ControlH() wide.
float SplitChipWidth(const char* label, bool bold, float label_max)
{
    float tw;
    if (bold) {
        FontScope f(FontRole::Semibold);
        tw = TextWidth(label);
    } else {
        tw = TextWidth(label);
    }
    if (label_max > 0.0f) tw = std::min(tw, label_max);
    return Dp(6.0f) + Dp(11.0f) + Dp(4.0f) + tw + Dp(6.0f) + ControlH();
}

SplitChipClick SplitChip(const char* id_label, const Icon& gear, bool on, bool selected, bool flag, bool bold,
                         float label_max)
{
    SplitChipClick out;
    const float h = ControlH();
    const float gw = h;
    const float avail = ImGui::GetContentRegionAvail().x;
    float       w = SplitChipWidth(id_label, bold, label_max);
    if (!WindowAutoSizes() && w > avail) w = std::max(gw + h, avail);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float  main_w = std::max(1.0f, w - gw);
    ImGui::PushID(id_label);
    ImGui::BeginGroup();
    out.main = ImGui::InvisibleButton("##main", ImVec2(main_w, h));
    out.main_hovered = ImGui::IsItemHovered();
    ImGui::SameLine(0.0f, 0.0f);
    out.gear = ImGui::InvisibleButton("##gear", ImVec2(gw, h));
    out.gear_hovered = ImGui::IsItemHovered();
    ImGui::EndGroup();
    ImGui::PopID();
    const bool   dis = ItemDisabled();
    const ImVec2 b(a.x + w, a.y + h);
    const ImVec2 split(a.x + main_w, a.y);
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const bool   crisp = PushCrisp();
    const float  r = h * 0.5f;
    DrawChipPill(dl, a, b, on, false);
    // Each zone's hover fill, clipped to its half of the pill.
    const ImU32 hov = on ? IM_COL32(0x5B, 0x8C, 0xFF, 0x40) : kHover;
    if (out.main_hovered) dl->AddRectFilled(a, ImVec2(split.x, b.y), Ink(hov), r, ImDrawFlags_RoundCornersLeft);
    if (selected)
        dl->AddRectFilled(split, b, Ink(kAccent), r, ImDrawFlags_RoundCornersRight);
    else if (out.gear_hovered)
        dl->AddRectFilled(split, b, Ink(hov), r, ImDrawFlags_RoundCornersRight);
    dl->AddLine(ImVec2(split.x + 0.5f, a.y + 1.0f), ImVec2(split.x + 0.5f, b.y - 1.0f),
                Ink(selected ? kAccent : (on ? kAccentLine : kStroke)));
    if (selected) dl->AddRect(ImVec2(a.x - 1.5f, a.y - 1.5f), ImVec2(b.x + 1.5f, b.y + 1.5f), Ink(kAccent), r + 1.5f, 0, Dp(1.5f));
    const ImU32 ink = Ink(ChipInk(on, out.main_hovered, dis));
    // The tick slot: always reserved, ticked when on.
    const float box = Dp(11.0f);
    if (on) DrawTick(dl, ImVec2(a.x + Dp(6.0f), a.y + (h - box) * 0.5f), box, ink);
    const std::string text(id_label, LabelEnd(id_label));
    const float       tx = a.x + Dp(6.0f) + box + Dp(4.0f);
    bool              cut = false;
    {
        ImFont*     font = bold ? Font(FontRole::Semibold) : ImGui::GetFont();
        const float fs = bold ? font->FontSize : ImGui::GetFontSize();
        const float ty = a.y + (h - fs) * 0.5f;
        const float max_x = split.x - Dp(6.0f);
        const float tw = font->CalcTextSizeA(fs, 1e9f, 0.0f, text.c_str()).x;
        if (tx + tw <= max_x + 0.5f) {
            dl->AddText(font, fs, ImVec2(tx, ty), ink, text.c_str());
        } else if (bold) {
            FontScope f(FontRole::Semibold);
            cut = EllipsisText(dl, ImVec2(tx, ty), max_x, ink, text);
        } else {
            cut = EllipsisText(dl, ImVec2(tx, ty), max_x, ink, text);
        }
    }
    // The ⚙ zone.
    Icon g = gear;
    g.col = selected ? kWhite : (out.gear_hovered ? kText : (on ? kAccentInk : kFaint));
    PaintIconAt(dl, g, split.x + (gw - IconPx()) * 0.5f, a.y, h);
    if (flag) dl->AddCircleFilled(ImVec2(b.x - Dp(6.0f), a.y + Dp(5.0f)), Dp(2.5f), Ink(kWarn));
    PopCrisp(crisp);
    if (cut && out.main_hovered) ShowTip(text.c_str());
    return out;
}

bool HelpMark(const char* id, const char* tip, float line_h)
{
    const float s = Dp(18.0f);
    const float h = std::max(s, line_h > 0.0f ? line_h : ImGui::GetFrameHeight());
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

float DrawSmallTagTone(ImDrawList* dl, ImVec2 p, const char* text, ImU32 ink, ImU32 line, ImU32 fill)
{
    const float  w = SmallTagWidth(text), h = Dp(17.0f);
    const ImVec2 b(p.x + w, p.y + h);
    dl->AddRectFilled(p, b, Ink(fill), Dp(4.0f));
    dl->AddRect(p, b, Ink(line), Dp(4.0f));
    ImFont* f = FontTiny();
    AddTextIn(dl, f, ImVec2(p.x + Dp(5.0f), std::floor(p.y + (h - f->FontSize) * 0.5f)), Ink(ink), text);
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

float ToneButtonWidth(const char* label, const Icon* icon)
{
    return Dp(8.0f) + TextWidth(label) + (icon && icon->fn ? Dp(6.0f) + IconPx() : 0.0f) + Dp(8.0f);
}

bool ToneButton(const char* label, TagTone tone, const Icon* icon, const ImVec2& size)
{
    const float  pad = Dp(8.0f);
    const float  h = size.y > 0.0f ? size.y : ControlH();
    const bool   has_icon = icon && icon->fn;
    const float  icon_room = has_icon ? Dp(6.0f) + IconPx() : 0.0f;
    const float  nat = ToneButtonWidth(label, icon);
    const float  avail = ImGui::GetContentRegionAvail().x;
    float        w = size.x > 0.0f ? size.x : (size.x < 0.0f ? std::max(1.0f, avail + size.x) : nat);
    if (!WindowAutoSizes() && w > avail) w = std::max(h, avail);
    const bool   clicked = ImGui::InvisibleButton(label, ImVec2(std::max(1.0f, w), h));
    const bool   hovered = ImGui::IsItemHovered();
    const bool   dis = ItemDisabled();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const bool   crisp = PushCrisp();
    const TagColours c = ToneColours(tone);
    dl->AddRectFilled(a, b, Ink(hovered ? kHover : kRaised), kRadiusSm);
    dl->AddRect(a, b, Ink(dis ? kStroke : c.edge), kRadiusSm);
    const std::string text(label, LabelEnd(label));
    const float       ty = a.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    const bool cut = EllipsisText(dl, ImVec2(a.x + pad, ty), b.x - pad - icon_room, Ink(dis ? kFaint : c.text), text);
    if (has_icon) {
        Icon ic = *icon;
        if (dis) ic.col = kFaint;
        PaintIconAt(dl, ic, b.x - pad - IconPx(), a.y, h);
    }
    PopCrisp(crisp);
    if (cut && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ShowTip(text.c_str());
    return clicked;
}

bool Switch(const char* id, bool on)
{
    // .sw: 26 x 16, the thumb 2 px inside its track.
    const float  w = Dp(26.0f), h = Dp(16.0f);
    const float  fh = ImGui::GetFrameHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  y = p0.y + std::floor((fh - h) * 0.5f);
    // Toggled on release over it (ImGui's button), or by the keyboard / gamepad (nav activation).
    const bool  toggled = ImGui::InvisibleButton(id, ImVec2(w, fh));
    const bool  hovered = ImGui::IsItemHovered();
    const bool  dis = ItemDisabled();
    const bool  crisp = PushCrisp();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32       track = on ? (hovered ? kPrimaryHover : kAccent) : (hovered ? kFrameHover : kStrokeStrong);
    if (dis) track = on ? WithAlpha(kAccent, 0x59) : kStroke;  // faint, the thumb still says on / off
    dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + w, y + h), Ink(track), h * 0.5f);
    dl->AddCircleFilled(ImVec2(on ? p0.x + w - h * 0.5f : p0.x + h * 0.5f, y + h * 0.5f), h * 0.5f - Dp(2.0f),
                        Ink(dis ? kFaint : kKnob), 12);
    PopCrisp(crisp);
    ImGui::RenderNavCursor(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), ImGui::GetItemID());
    return toggled;
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

void ToneText(const char* text, ImU32 col, bool wrap, float wrap_w)
{
    if (!text) return;
    ImGui::PushStyleColor(ImGuiCol_Text, Col(col));
    if (wrap) ImGui::PushTextWrapPos(wrap_w > 0.0f ? ImGui::GetCursorPosX() + wrap_w : 0.0f);
    ImGui::TextUnformatted(text);
    if (wrap) ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void ToneText(const std::string& text, ImU32 col, bool wrap, float wrap_w)
{
    ToneText(text.c_str(), col, wrap, wrap_w);
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

void FormatNumber(char* out, size_t n, double v, int decimals, bool trim, bool sign = false)
{
    if (sign && RoundTo(v, decimals) > 0.0 && n > 1) {  // "+3" (0 stays "0", as the mockup's sgn)
        out[0] = '+';
        FormatNumber(out + 1, n - 1, v, decimals, trim, false);
        return;
    }
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
                           bool trim, ImU32 text_col, bool sign)
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
    FormatNumber(text, sizeof(text), *value, decimals, trim, sign);

    if (g_dn.typing == gid || g_dn.drag == gid) g_dn.seen_frame = ImGui::GetFrameCount();
    if (g_dn.typing == gid) {
        // Typing: the same raised field, its edge accent-line (the focus ring), the number in mono.
        ImGui::PushFont(mono);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col(kRaised));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Col(kRaised));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Col(kRaised));
        ImGui::PushStyleColor(ImGuiCol_Border, Col(kAccentLine));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Dp(kRadiusSm));
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
        dl->AddRectFilled(p0, p1, Ink(kRaised), Dp(kRadiusSm));
        dl->AddRect(p0, p1, Ink((hovered || active) ? kStrokeStrong : kStroke), Dp(kRadiusSm));
        FormatNumber(text, sizeof(text), *value, decimals, trim, sign);
        const ImVec2 ts = TextSizeIn(mono, text);
        dl->PushClipRect(p0, p1, true);
        AddTextIn(dl, mono, ImVec2(std::max(p0.x + Dp(3.0f), p1.x - Dp(5.0f) - ts.x), std::floor(p0.y + (h - ts.y) * 0.5f)),
                  Ink(text_col ? text_col : kText), text);
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

namespace {
int g_fader_typing_frame = -1;  // the last frame a fader was typed into
// The fader whose typing Esc ended, and that frame (FaderCancelled).
ImGuiID g_fader_cancel_id = 0;
int     g_fader_cancel_frame = -1;
}  // namespace

bool FaderTyping() { return g_fader_typing_frame >= ImGui::GetFrameCount() - 1; }

bool FaderCancelled()
{
    return g_fader_cancel_frame == ImGui::GetFrameCount() && g_fader_cancel_id != 0 &&
           g_fader_cancel_id == ImGui::GetItemID();
}

bool Fader(const char* id, float* v, float v_min, float v_max, float width)
{
    return Fader(id, v, v_min, v_max, width, FaderOpts());
}

bool Fader(const char* id, float* v, float v_min, float v_max, float width, const FaderOpts& o)
{
    if (!v) return false;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool    has_next_w = (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth) != 0;
    const float   avail = ImGui::GetContentRegionAvail().x;
    float         w = width > 0.0f ? width : (has_next_w ? ImGui::CalcItemWidth() : avail);
    if (!WindowAutoSizes()) w = std::min(w, avail);
    w = std::max(w, Dp(20.0f));
    const float h = ControlH();
    // 13-14 (G-13): the value right of the track (.fader: gap 8), when it leaves the track 20 px.
    ImFont*     mono = FontMonoSmall();
    const float gap = Dp(8.0f);
    float       value_w = 0.0f;
    char        text[64] = {};
    if (o.format && o.format[0]) {
        value_w = o.value_w;
        if (value_w <= 0.0f) {
            std::snprintf(text, sizeof(text), o.format, static_cast<double>(v_min));
            value_w = TextSizeIn(mono, text).x;
            std::snprintf(text, sizeof(text), o.format, static_cast<double>(v_max));
            value_w = std::max(value_w, TextSizeIn(mono, text).x) + Dp(2.0f);
        }
        if (w - gap - value_w < Dp(20.0f)) value_w = 0.0f;
    }
    const float track_w = value_w > 0.0f ? w - gap - value_w : w;
    // 13-14 (finishing): Ctrl+click types a value, as ImGui's slider does (TempInputScalar): the field
    // takes the track's id, so the gesture stays one active item for IsItemActive / IsItemDeactivated.
    const ImGuiID gid = ImGui::GetID(id);
    bool          typing = ImGui::TempInputIsActive(gid);
    bool          hovered = false, active = false;
    const float   v_start = *v;  // the value at this frame's start
    // The value typing began with, kept by the fader's id: Esc restores it exactly (ImGui's field
    // would restore its rounded text, "-14.54" for -14.537).
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID saved_key = ImHashStr("##fader_saved", 0, gid);
    if (typing) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const ImRect bb(c, ImVec2(c.x + track_w, c.y + h));
        ImGui::ItemSize(bb);
        ImGui::ItemAdd(bb, gid);
    } else {
        ImGui::InvisibleButton(id, ImVec2(track_w, h));
        hovered = ImGui::IsItemHovered();
        active = ImGui::IsItemActive();
        typing = ImGui::IsItemActivated() && ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (typing) store->SetFloat(saved_key, *v);
    }
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float  r = Dp(8.0f);  // the 16 px thumb
    bool         changed = false;
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    if (typing) {
        // The DragNumber's typing look: raised, accent-line edge, r5, the number in mono 11.
        g_fader_typing_frame = ImGui::GetFrameCount();
        ImGui::PushFont(mono);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Col(kRaised));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Col(kRaised));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Col(kRaised));
        ImGui::PushStyleColor(ImGuiCol_Border, Col(kAccentLine));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Dp(kRadiusSm));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(5.0f), std::max(0.0f, (h - mono->FontSize) * 0.5f)));
        const float lo = std::min(v_min, v_max), hi = std::max(v_min, v_max);
        const bool  esc = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        changed = ImGui::TempInputScalar(ImRect(a, b), gid, id, ImGuiDataType_Float, v,
                                         (o.format && o.format[0]) ? o.format : "%.2f", &lo, &hi);
        if (esc && !ImGui::TempInputIsActive(gid)) {
            *v = store->GetFloat(saved_key, v_start);
            changed = *v != v_start;
            g_fader_cancel_id = gid;
            g_fader_cancel_frame = ImGui::GetFrameCount();
        }
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(4);
        ImGui::PopFont();
    } else {
        if (active) {
            const float nv = flow::FaderValueAt(ImGui::GetIO().MousePos.x, a.x, b.x, r, v_min, v_max, o.log);
            if (nv != *v) {
                *v = nv;
                changed = true;
            }
        }
        const float cy = std::floor((a.y + b.y) * 0.5f) + 0.5f;
        const float th = Dp(6.0f);  // the track
        const float tx = flow::FaderThumbX(*v, a.x, b.x, r, v_min, v_max, o.log);
        dl->AddRectFilled(ImVec2(a.x, cy - th * 0.5f), ImVec2(b.x, cy + th * 0.5f), Ink(kStrokeStrong), th * 0.5f);
        if (tx > a.x) dl->AddRectFilled(ImVec2(a.x, cy - th * 0.5f), ImVec2(tx, cy + th * 0.5f), Ink(kAccent), th * 0.5f);
        if (o.mark_on) {  // 13-74
            const float mx = std::round(flow::FaderThumbX(o.mark, a.x, b.x, r, v_min, v_max, o.log));
            dl->AddRectFilled(ImVec2(mx - Dp(1.0f), cy - Dp(8.0f)), ImVec2(mx + Dp(1.0f), cy + Dp(8.0f)), Ink(kWarn), Dp(1.0f));
        }
        dl->AddCircleFilled(ImVec2(tx, cy), r, Ink((hovered || active) ? kPrimaryHover : kAccent), 24);
    }
    if (value_w > 0.0f) {
        // The value takes its room on the line without becoming the last item (ItemSize only), so
        // IsItemActive / IsItemDeactivated after the fader still read the track.
        std::snprintf(text, sizeof(text), o.format, static_cast<double>(*v));
        const ImVec2 ts = TextSizeIn(mono, text);
        AddTextIn(dl, mono, ImVec2(b.x + gap + value_w - ts.x, std::floor(a.y + (h - ts.y) * 0.5f)), Ink(kText), text);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::ItemSize(ImVec2(gap + value_w, h));
    }
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
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Dp(kRadiusSm));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(w);
    const bool r = hint ? ImGui::InputTextWithHint(id, hint, buf, buf_size, flags) : ImGui::InputText(id, buf, buf_size, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    const bool   hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    dl->AddRect(a, b, Ink(active ? kAccentLine : (hovered ? kStrokeStrong : kStroke)), Dp(kRadiusSm));
    if (active) {  // 13-14 (B7): outline 2px accent-line, outline-offset 1px
        const float o = Dp(2.0f);
        dl->AddRect(ImVec2(a.x - o, a.y - o), ImVec2(b.x + o, b.y + o), Ink(kAccentLine), Dp(kRadiusSm) + o, 0, Dp(2.0f));
    }
    return r;
}

bool TitleField(const char* id, char* buf, size_t buf_size, float width, int flags, ImFont* font)
{
    bool hovered = false, active = false;
    FieldState(id, &hovered, &active);
    const float w = FieldWidth(width);
    ImFont*     f = font ? font : FontHeading();
    // 28 px for the heading; an inline field in another font: the font plus 6, never under ControlH().
    const float h = font ? std::max(ControlH(), std::floor(f->FontSize + Dp(6.0f))) : Dp(28.0f);
    ImGui::PushFont(f);
    const ImU32 fill = (hovered || active) ? kBg : IM_COL32(0, 0, 0, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col(fill));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Col(fill));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Col(fill));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(6.0f), std::max(0.0f, (h - f->FontSize) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Dp(kRadiusSm));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(w);
    const bool r = ImGui::InputText(id, buf, buf_size, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Ink(kStrokeStrong), Dp(kRadiusSm));
    return r;
}

bool RenameField(const char* id, char* buf, size_t buf_size, float width, int flags, const char* hint)
{
    const float w = FieldWidth(width);
    ImFont*     f = FontTitle();
    ImGui::PushFont(f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col(kBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Col(kBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Col(kBg));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Col(kFaint));  // the hint
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(6.0f), std::max(0.0f, (ControlH() - f->FontSize) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Dp(kRadiusSm));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(w);
    const bool r = hint ? ImGui::InputTextWithHint(id, hint, buf, buf_size, flags) : ImGui::InputText(id, buf, buf_size, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    ImGui::PopFont();
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), Ink(kAccentLine), Dp(kRadiusSm));
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
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Dp(kRadiusSm));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(w);
    const bool r = ImGui::InputTextWithHint(id, hint ? hint : "", buf, buf_size, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (ImGui::IsItemActive() || ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddRect(a, b, Ink(kStrokeStrong), Dp(kRadiusSm));
    else DashedRect(ImGui::GetWindowDrawList(), a, b, Ink(kStrokeStrong), Dp(kRadiusSm));
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

void WindowShadow(float alpha)
{
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (!win || alpha <= 0.0f) return;
    const ImVec2 a = win->Pos, b(win->Pos.x + win->Size.x, win->Pos.y + win->Size.y);
    if (b.x - a.x < 2.0f || b.y - a.y < 2.0f) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2         v0 = vp->Pos, v1(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    // 13-47: a soft, tight shadow (Antho: the mockup's 0 18px 50px read as a dark slab): the window's
    // rect moved 4 px down, fading over 10 px past it, at most half `alpha` dark at its edge. Ten
    // 1 px layers whose alphas stack to a smooth (1 - t)^2 falloff (flow::ShadowLayerAlphas).
    constexpr int kLayers = 10;
    const float   off = Dp(4.0f), spread = Dp(10.0f);
    float         layer_a[kLayers];
    flow::ShadowLayerAlphas(alpha * 0.5f, kLayers, layer_a);
    const float   r = win->WindowRounding;
    ImDrawList*   dl = win->DrawList;
    // Outside the window only (above, below, left, right of it): its own content stays untouched.
    const ImVec4 clips[4] = {ImVec4(v0.x, v0.y, v1.x, a.y), ImVec4(v0.x, b.y, v1.x, v1.y), ImVec4(v0.x, a.y, a.x, b.y),
                             ImVec4(b.x, a.y, v1.x, b.y)};
    for (const ImVec4& c : clips) {
        if (c.z <= c.x || c.w <= c.y) continue;
        dl->PushClipRect(ImVec2(c.x, c.y), ImVec2(c.z, c.w), false);
        for (int i = 0; i < kLayers; ++i) {
            const int la = static_cast<int>(layer_a[i] * 255.0f + 0.5f);
            if (la <= 0) continue;
            const float s = spread * static_cast<float>(i + 1) / static_cast<float>(kLayers);
            dl->AddRectFilled(ImVec2(a.x - s, a.y + off - s), ImVec2(b.x + s, b.y + off + s), IM_COL32(0, 0, 0, la), r + s);
        }
        dl->PopClipRect();
    }
}

bool BeginMenuPopup(const char* id, int window_flags)
{
    PushMenuStyle();
    ImGui::SetNextWindowSizeConstraints(ImVec2(Dp(220.0f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopup(id, window_flags)) {
        PopMenuStyle();
        return false;
    }
    WindowShadow();
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
    WindowShadow();
    return true;
}

bool BeginSubMenu(const char* label, bool enabled)
{
    ImGui::SetNextWindowSizeConstraints(ImVec2(Dp(220.0f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginMenu(label, enabled)) return false;
    WindowShadow();
    return true;
}

void MenuSeparator()
{
    // .pop .sep: 1 px kStroke, 4 px above and under it, 2 px in from the row's edges.
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float  x0 = win->WorkRect.Min.x - ImGui::GetStyle().WindowPadding.x + Dp(4.0f) + Dp(2.0f);
    const float  x1 = win->WorkRect.Max.x + ImGui::GetStyle().WindowPadding.x - Dp(4.0f) - Dp(2.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  sp = ImGui::GetStyle().ItemSpacing.y;
    // The rows' highlights end half a spacing above the cursor and the next one starts half a spacing
    // under this item: the line sits in the middle of the 9 px (4 + 1 + 4) between them.
    const float  h = std::max(0.0f, Dp(9.0f) - sp);
    const float  y = std::floor(p.y + h * 0.5f) + 0.5f;
    win->DrawList->AddLine(ImVec2(x0, y), ImVec2(std::max(x0, x1), y), Ink(kStroke), 1.0f);
    ImGui::Dummy(ImVec2(1.0f, h));
}

bool BeginPanelPopup(const char* id, int window_flags, ImVec2 pad)
{
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Col(kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStrokeStrong));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(pad.x), Dp(pad.y)));
    const bool open = ImGui::BeginPopup(id, window_flags);
    ImGui::PopStyleVar();  // read when the window begins
    ImGui::PopStyleColor(2);
    if (open) {
        WindowShadow();
        PushThemeMetrics();  // not a toolbar's / menu's metrics around it
    }
    return open;
}

void EndPanelPopup()
{
    PopThemeMetrics();
    ImGui::EndPopup();
}

bool BeginModalCard(const char* id, int window_flags, float width)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float          margin = Dp(32.0f);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             window_flags;
    // Never taller (nor wider) than the viewport less the margin, sized to its content or not: the
    // content scrolls.
    const ImVec2 cap(std::max(Dp(120.0f), vp->Size.x - margin), std::max(Dp(120.0f), vp->Size.y - margin));
    if (width > 0.0f) {
        ImGui::SetNextWindowSize(ImVec2(std::max(Dp(120.0f), std::min(width, vp->Size.x - margin)), 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, cap.y));
        flags |= ImGuiWindowFlags_NoResize;
    } else {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), cap);
        flags |= ImGuiWindowFlags_AlwaysAutoResize;
    }
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Col(kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStrokeStrong));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(16.0f), Dp(14.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(kRadiusMd));
    const bool open = ImGui::BeginPopupModal(id, nullptr, flags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (open) {
        WindowShadow();
        PushThemeMetrics();
    }
    return open;
}

void EndModalCard()
{
    PopThemeMetrics();
    ImGui::EndPopup();
}

bool BeginSheetModal(const char* id, ImVec2 pos, ImVec2 size, bool* backdrop_clicked)
{
    if (backdrop_clicked) *backdrop_clicked = false;
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Col(kSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStrokeStrong));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, Col(IM_COL32(0, 0, 0, 0x80)));  // read as the window begins
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Dp(kRadiusLg));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    const bool open = ImGui::BeginPopupModal(id, nullptr, flags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    if (!open) return false;
    WindowShadow(0.7f);
    PushThemeMetrics();
    if (backdrop_clicked) {
        // Nothing open over it, now or last frame (a menu or a modal of its own takes that click: the click
        // that closes a menu outside it does not close the sheet too).
        const bool    over = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        ImGuiStorage* st = ImGui::GetStateStorage();
        const ImGuiID key = ImGui::GetID("##sheet_over");
        const bool    was_over = st->GetBool(key, false);
        st->SetBool(key, over);
        const ImVec2 a = ImGui::GetWindowPos(), b(a.x + ImGui::GetWindowWidth(), a.y + ImGui::GetWindowHeight());
        *backdrop_clicked = !over && !was_over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                            !ImGui::IsMouseHoveringRect(a, b, false);
    }
    return true;
}

void EndSheetModal()
{
    PopThemeMetrics();
    ImGui::EndPopup();
}

bool BeginFloatingCard(const char* id, ImVec2 pos, ImVec2 pivot, float width)
{
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    if (width > 0.0f) ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStrokeStrong));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(16.0f), Dp(14.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Dp(kRadiusMd));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoFocusOnAppearing;
    if (width <= 0.0f) flags |= ImGuiWindowFlags_AlwaysAutoResize;
    const bool shown = ImGui::Begin(id, nullptr, flags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (shown) WindowShadow();
    PushThemeMetrics();  // whatever Begin returned: EndFloatingCard always pops them
    return shown;
}

void EndFloatingCard()
{
    PopThemeMetrics();
    ImGui::End();
}

bool BeginMovableCard(const char* id, int window_flags)
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStrokeStrong));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(8.0f), Dp(8.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Dp(kRadiusMd));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse | window_flags;
    const bool shown = ImGui::Begin(id, nullptr, flags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (shown) WindowShadow();
    PushThemeMetrics();  // whatever Begin returned: EndMovableCard always pops them
    return shown;
}

void EndMovableCard()
{
    PopThemeMetrics();
    ImGui::End();
}

bool BeginToolWindowFill(const char* id)
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStrokeStrong));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(8.0f), Dp(8.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    const bool shown = ImGui::Begin(id, nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    PushThemeMetrics();  // whatever Begin returned: EndToolWindowFill always pops them
    return shown;
}

void EndToolWindowFill()
{
    PopThemeMetrics();
    ImGui::End();
}

bool BeginOverlay(const char* id, ImVec2 pos, ImVec2 pivot, int extra_flags)
{
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoBackground | extra_flags;
    const bool shown = ImGui::Begin(id, nullptr, flags);
    ImGui::PopStyleVar(2);
    return shown;
}

void EndOverlay() { ImGui::End(); }

bool BeginOverlayPanel(const char* id, ImVec2 pos, ImVec2 size, int extra_flags)
{
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(kBg));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStrokeStrong));
    // 13-53: pad 0 (the content sets its own insets, its body runs edge to edge), kept for the child
    // windows inside, with the theme's spacing and frame padding: never the caller's (the Library's
    // panes push a 0 padding around the code that opens Edit).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Dp(kRadiusLg));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoMove | extra_flags;
    const bool shown = ImGui::Begin(id, nullptr, flags);
    ImGui::PopStyleVar();  // WindowRounding
    PushThemeMetrics();
    ImGui::PopStyleColor(2);
    if (shown) WindowShadow(0.7f);  // a little heavier than a menu (the mockup's .edbox .7)
    return shown;
}

void EndOverlayPanel()
{
    PopThemeMetrics();
    ImGui::PopStyleVar();  // WindowPadding
    ImGui::End();
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

bool BeginComboStyled(const char* id, const char* preview, float width, int flags, TagTone tone, bool follow)
{
    const bool bad = tone == TagTone::Bad;
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
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Dp(kRadiusSm));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, follow ? 0.0f : 1.0f);  // Border = stroke-strong (PushMenuWindowStyle)
    if (follow) {  // 13-62: see-through (the hover tint kept), its edge dashed below
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    }
    ImGui::SetNextItemWidth(w);
    const bool open = ImGui::BeginCombo(id, nullptr, flags | ImGuiComboFlags_NoArrowButton);
    if (follow) ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (open) {
        PushMenuItemStyle();
        WindowShadow();
    }
    // The preview, ellipsized before the chevron (a "v" in the text colour).
    const ImVec2 p1(p0.x + w, p0.y + FieldH());
    if (bad) dl->AddRect(p0, p1, Ink(kBadLine), Dp(kRadiusSm));  // 13-14 (G-6): a missing value
    else if (follow) DashedRect(dl, p0, p1, Ink(kStrokeStrong), Dp(kRadiusSm));
    if (preview && preview[0]) {
        const std::string s(preview);
        EllipsisText(dl, ImVec2(p0.x + Dp(6.0f), p0.y + (FieldH() - ImGui::GetTextLineHeight()) * 0.5f), p1.x - chev - Dp(2.0f),
                     Ink(bad ? kBadInk : follow ? kMuted : kText), s);
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

float ChipComboWidth(const char* preview, float min_w)
{
    return std::max(min_w > 0.0f ? min_w : Dp(40.0f), (preview ? ImGui::CalcTextSize(preview).x : 0.0f) + Dp(30.0f));
}

bool BeginChipCombo(const char* id, const char* preview, float min_w, bool disabled, int flags, float right)
{
    const float  mw = min_w > 0.0f ? min_w : Dp(40.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    float        avail = ImGui::GetContentRegionAvail().x;
    if (right > 0.0f) avail = std::min(avail, right - p0.x);
    const float w = WindowAutoSizes() ? ChipComboWidth(preview, mw) : std::min(ChipComboWidth(preview, mw), std::max(mw, avail));
    const float h = ImGui::GetFrameHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // The list's popup style now (read when it begins); its rows' spacing once it is open.
    PushMenuWindowStyle();
    ImGui::SetNextItemWidth(w);
    if (disabled) {  // really disabled (never hovered, never opens), without ImGui's fade
        ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);  // read by BeginDisabled only
        ImGui::BeginDisabled(true);
        ImGui::PopStyleVar();
    }
    bool open = ImGui::BeginCombo(id, nullptr, flags | ImGuiComboFlags_NoArrowButton);
    if (open && disabled) {  // its list was open when it became disabled: closed now
        ImGui::CloseCurrentPopup();
        ImGui::EndCombo();
        open = false;
    }
    if (disabled) ImGui::EndDisabled();
    if (open) {
        PushMenuItemStyle();
        WindowShadow();
    }
    // The preview (ellipsized) and the small chevron (select.chip).
    const bool  crisp = PushCrisp();
    const ImU32 ink = Ink(disabled || ItemDisabled() ? kFaint : kText);
    if (preview && preview[0])
        EllipsisText(dl, ImVec2(p0.x + ImGui::GetStyle().FramePadding.x, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                     p0.x + w - Dp(18.0f), ink, std::string(preview));
    const ImVec2 c(p0.x + w - Dp(10.0f), p0.y + h * 0.5f);
    dl->AddTriangleFilled(ImVec2(c.x - Dp(3.5f), c.y - Dp(1.5f)), ImVec2(c.x + Dp(3.5f), c.y - Dp(1.5f)),
                          ImVec2(c.x, c.y + Dp(2.5f)), Ink(disabled || ItemDisabled() ? kFaint : kMuted));
    PopCrisp(crisp);
    if (!open) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    return open;
}

void EndChipCombo()
{
    ImGui::EndCombo();
    PopMenuStyle();
}

bool SelectField(const char* id, const char* label, const char* value, const SelectFieldOpts& o)
{
    const float  h = FieldH();
    const float  avail = ImGui::GetContentRegionAvail().x;
    const float  w = std::max(h, o.width > 0.0f ? (WindowAutoSizes() ? o.width : std::min(o.width, avail)) : avail);
    const bool   clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool   hovered = ImGui::IsItemHovered();
    const bool   dis = ItemDisabled();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const bool   crisp = PushCrisp();
    dl->AddRectFilled(a, b, Ink(hovered ? kHover : kRaised), kRadiusSm);
    dl->AddRect(a, b, Ink(o.open ? kAccentLine : kStroke), kRadiusSm);
    const float ty = a.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    float       x = a.x + Dp(8.0f);
    const float chev_x = b.x - Dp(10.0f);
    const float text_r = chev_x - Dp(8.0f);  // nothing runs into the chevron
    bool        cut = false;
    const std::string label_s = label ? std::string(label, LabelEnd(label)) : std::string();
    if (!label_s.empty()) {
        // Narrow, the label keeps at most half the room when a value follows it, and is ellipsized.
        const float lw = ImGui::CalcTextSize(label_s.c_str()).x;
        const float room = std::max(0.0f, text_r - x);
        const float lmax = (value && value[0]) ? std::max(room * 0.5f, std::min(lw, room - Dp(40.0f))) : room;
        cut = EllipsisText(dl, ImVec2(x, ty), x + std::min(lw, lmax), Ink(dis ? kFaint : kMuted), label_s) || cut;
        x += std::min(lw, lmax) + Dp(6.0f);
    }
    if (o.icon && o.icon->fn) {
        Icon ic = *o.icon;
        if (dis) ic.col = kFaint;
        PaintIconAt(dl, ic, x, a.y, h, Dp(10.0f));
        x += Dp(12.0f);
    }
    // The tag at the right, before the chevron; the value ellipsized before it.
    float tag_x = chev_x - Dp(8.0f);
    if (o.tag && o.tag[0]) {
        const float tw = ImGui::CalcTextSize(o.tag).x;
        tag_x = chev_x - Dp(8.0f) - tw;
        if (tag_x > x + Dp(24.0f))
            dl->AddText(ImVec2(tag_x, ty), Ink(dis ? kFaint : (o.tag_tone == TagTone::Neutral ? kFaint : ToneColours(o.tag_tone).text)),
                        o.tag);
        else tag_x = chev_x - Dp(8.0f);
    }
    if (value && value[0] && tag_x - Dp(6.0f) > x)
        cut = EllipsisText(dl, ImVec2(x, ty), tag_x - Dp(6.0f), Ink(dis ? kFaint : (o.value_col ? o.value_col : kText)),
                           std::string(value)) || cut;
    else if (value && value[0]) cut = true;  // no room left for it at all
    const ImVec2 c(chev_x, a.y + h * 0.5f);
    dl->AddTriangleFilled(ImVec2(c.x - Dp(3.5f), c.y - Dp(2.0f)), ImVec2(c.x + Dp(3.5f), c.y - Dp(2.0f)),
                          ImVec2(c.x, c.y + Dp(2.5f)), Ink(dis ? kFaint : kMuted));
    PopCrisp(crisp);
    if (cut && !o.open && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        std::string tip = label_s;
        if (value && value[0]) tip += (tip.empty() ? "" : " ") + std::string(value);
        ShowTip(tip.c_str());
    }
    return clicked;
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
        DrawTick(dl, b0, box, Ink(kWhite));
    } else {
        dl->AddRectFilled(b0, b1, Ink(kBg), r);
        dl->AddRect(b0, b1, Ink(hovered ? kFrameHover : kStrokeStrong), r);
    }
    if (has_text)
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + box + gap, a.y + (fh - ImGui::GetTextLineHeight()) * 0.5f),
                    Ink(kText), label, end, wrap);
    return clicked;
}

bool TriCheckbox(const char* id, int state)
{
    const float box = Dp(14.0f);
    const float fh = ImGui::GetFrameHeight();
    const bool  clicked = ImGui::InvisibleButton(id, ImVec2(box, std::max(fh, box)));
    const bool  hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  by = std::floor(a.y + (std::max(fh, box) - box) * 0.5f);
    const ImVec2 b0(a.x, by), b1(a.x + box, by + box);
    const float  r = Dp(3.0f), k = box / 14.0f;
    if (state != 0) {
        dl->AddRectFilled(b0, b1, Ink(hovered ? kPrimaryHover : kAccent), r);
        if (state == 2) {  // half-ticked: a dash
            const float y = std::floor(b0.y + box * 0.5f) + 0.5f;
            dl->AddLine(ImVec2(b0.x + 3.5f * k, y), ImVec2(b1.x - 3.5f * k, y), Ink(kWhite), std::max(1.5f, 1.8f * k));
        } else {
            const ImVec2 tick[3] = {ImVec2(b0.x + 3.2f * k, b0.y + 7.2f * k), ImVec2(b0.x + 5.9f * k, b0.y + 9.9f * k),
                                    ImVec2(b0.x + 10.8f * k, b0.y + 4.2f * k)};
            dl->AddPolyline(tick, 3, Ink(kWhite), ImDrawFlags_None, std::max(1.5f, 1.8f * k));
        }
    } else {
        dl->AddRectFilled(b0, b1, Ink(kBg), r);
        dl->AddRect(b0, b1, Ink(hovered ? kFrameHover : kStrokeStrong), r);
    }
    return clicked;
}

ImU32 LibDot(unsigned int color)
{
    if (color & 0x1000000u) return IM_COL32((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, 0xFF);
    return kAccent;
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

void RowBackground(ImDrawList* dl, ImVec2 a, ImVec2 b, unsigned bits, float radius, ImU32 stripe)
{
    if (!dl) return;
    const bool sel = (bits & kRowSelected) != 0, lit = (bits & kRowLit) != 0, hov = (bits & kRowHovered) != 0;
    if (bits & kRowCard) {
        // 13-14 (finishing): the card row (10-3 fb-2's rule card): filled and edged, a stripe between.
        dl->AddRectFilled(a, b, Ink(hov && !sel ? kHover : kRaised), radius);
        if (sel) dl->AddRectFilled(a, b, Ink(kAccentSoft), radius);
        if (stripe) RuleStripe(dl, a, b, Ink(stripe), radius);
        dl->AddRect(a, b, Ink(sel ? kAccentLine : kStroke), radius);
        return;
    }
    if (bits & kRowEven) dl->AddRectFilled(a, b, Ink(kRowAlt), radius);
    if (sel || lit) {
        dl->AddRectFilled(a, b, Ink(kAccentSoft), radius);
    } else if (hov) {
        dl->AddRectFilled(a, b, Ink((bits & kRowHoverStrong) ? kHover : kRaised), radius);
    }
    if (stripe) RuleStripe(dl, a, b, Ink(stripe), radius);
    if (sel && !(bits & kRowBarSelect)) dl->AddRect(a, b, Ink(kAccentLine), radius);
    if ((sel && (bits & kRowBarSelect)) || lit) LeftBar(dl, a, b, Ink(kAccent), radius);
    else if (bits & kRowCurrent) LeftBar(dl, a, b, Ink(kAccentLine), radius);
}

bool SelectRow(const char* label, bool selected, unsigned bits, float height, int sflags, float radius, float width)
{
    const float h = height > 0.0f ? height : RowH();
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    ImGui::PushStyleColor(ImGuiCol_Header, Col(IM_COL32(0, 0, 0, 0)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Col(IM_COL32(0, 0, 0, 0)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, Col(IM_COL32(0, 0, 0, 0)));
    ImGui::PushStyleColor(ImGuiCol_Text, Col(IM_COL32(0, 0, 0, 0)));
    const bool clicked = ImGui::Selectable(label, selected, sflags | ImGuiSelectableFlags_NoPadWithHalfSpacing,
                                           ImVec2(width > 0.0f ? width : 0.0f, h));
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
            ShowTip(text.c_str());
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
    // Antho 2026-10-12: a thin chevron (an open row "v", a folded one ">"), not the filled triangle, which read as
    // the rows' Play button; the section headers keep their triangle (DrawFoldTriangle).
    const ImVec2 c(std::floor((a.x + b.x) * 0.5f) + 0.5f, std::floor((a.y + b.y) * 0.5f) + 0.5f);
    const float  r = Dp(3.0f), t = std::max(1.0f, Dp(1.25f));
    const ImU32  col = Ink(hovered ? kText : kMuted);
    if (open) {
        const ImVec2 pts[3] = {ImVec2(c.x - r, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x + r, c.y - r * 0.5f)};
        dl->AddPolyline(pts, 3, col, ImDrawFlags_None, t);
    } else {
        const ImVec2 pts[3] = {ImVec2(c.x - r * 0.5f, c.y - r), ImVec2(c.x + r * 0.5f, c.y), ImVec2(c.x - r * 0.5f, c.y + r)};
        dl->AddPolyline(pts, 3, col, ImDrawFlags_None, t);
    }
    return clicked;
}

// ---- dashed outline (13-7 B18) -----------------------------------------------------------

void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float radius, float dash, float gap, float thickness)
{
    if (!dl || b.x <= a.x || b.y <= a.y) return;
    dash = std::max(1.0f, Dp(dash));
    gap = std::max(1.0f, Dp(gap));
    // Freeze audit #1: a rectangle reaching far past the clip rect (a variation at deep zoom on a long file) is cut
    // first to the clip rect plus a margin, each cut a whole number of dash periods (the visible dashes keep their
    // phase): its outline used to be walked over its whole length, millions of dashes, and past ~1.3e8 px a float
    // step stopped moving at all.
    {
        const ImVec2 c0 = dl->GetClipRectMin(), c1 = dl->GetClipRectMax();
        if (!(b.x >= c0.x && a.x <= c1.x && b.y >= c0.y && a.y <= c1.y)) return;  // also NaN
        const double per = static_cast<double>(dash) + gap, m = 64.0 + std::max(0.0f, radius);
        auto cut = [&](float& lo, float& hi, float clo, float chi) {
            if (lo < clo - m) lo = static_cast<float>(lo + std::floor((clo - m - lo) / per) * per);
            if (hi > chi + m) hi = static_cast<float>(lo + std::ceil((chi + m - lo) / per) * per);  // steady while it scrolls
        };
        cut(a.x, b.x, c0.x, c1.x);
        cut(a.y, b.y, c0.y, c1.y);
    }
    // The rounded rectangle's outline as a polyline (ImGui's own path), then walked in dashes.
    dl->PathRect(ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f), radius);
    ImVector<ImVec2> pts = dl->_Path;
    dl->PathClear();
    if (pts.Size < 2) return;
    pts.push_back(pts[0]);
    bool  on = true;
    float left = dash;
    int   budget = 20000;  // hard cap on the steps walked (a screen's outline is a few thousand)
    for (int i = 0; i + 1 < pts.Size && budget > 0; ++i) {
        const ImVec2 p = pts[i], q = pts[i + 1];
        const float  dx = q.x - p.x, dy = q.y - p.y;
        const float  len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0f) continue;
        float t = 0.0f;
        while (t < len && --budget > 0) {
            const float stp = std::min(left, len - t);
            if (!(stp > 0.0f) || t + stp == t) break;
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Dp(kRadiusMd));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(14.0f), Dp(8.0f)));
    ImGui::PushFont(FontTitle());
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    const bool shown = ImGui::Begin(id, nullptr, kFlags);
    if (shown) WindowShadow(0.55f);  // a little lighter than a menu (the mockup's .toast .55)
    return shown;
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
    const ImVec2 b(p.x + ts.x + Dp(8.0f), p.y + ts.y + Dp(3.0f));
    dl->AddRectFilled(p, b, kBg, Dp(4.0f));
    dl->AddRect(p, b, kStrokeStrong, Dp(4.0f));
    dl->AddText(ImVec2(p.x + Dp(4.0f), p.y + Dp(1.5f)), text ? text : kText, key);
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

std::string EllipsizeToWidth(ImFont* font, const std::string& text, float max_w)
{
    if (!font) return text;
    auto width = [&](const std::string& t) { return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, t.c_str()).x; };
    if (width(text) <= max_w) return text;
    std::string t = text;
    while (!t.empty()) {
        // a whole character: its continuation bytes, then its lead byte
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();
        if (!t.empty()) t.pop_back();
        if (width(t + "...") <= max_w) return t + "...";
    }
    return std::string();
}

void VerticalText(ImDrawList* dl, ImFont* font, float cx, float top, ImU32 col, const std::string& text, bool down,
                  ImVec2 clip_min, ImVec2 clip_max)
{
    if (!dl || !font || text.empty()) return;
    const ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text.c_str());
    // Laid out flat at `o` (never culled: the clip holds the flat text and the strip), then each of
    // its vertices turned about it and moved into place.
    const ImVec2 o(cx, top);
    dl->PushClipRect(ImVec2(std::min(clip_min.x, o.x), std::min(clip_min.y, o.y)),
                     ImVec2(std::max(clip_max.x, o.x + ts.x), std::max(clip_max.y, o.y + ts.y)), false);
    const int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, font->FontSize, o, col, text.c_str());
    for (int k = v0; k < dl->VtxBuffer.Size; ++k) {
        ImVec2&     v = dl->VtxBuffer[k].pos;
        const float x = v.x - o.x, y = v.y - o.y;
        v = down ? ImVec2(cx + ts.y * 0.5f - y, top + x) : ImVec2(cx - ts.y * 0.5f + y, top + ts.x - x);
    }
    dl->PopClipRect();
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
    dl->PushClipRect(p0, ImVec2(p0.x + Dp(3.0f), p1.y), true);
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

// ---- panes, wells, bands (13-14 G-19 / G-2) ---------------------------------------------

bool BeginPane(const char* id, ImVec2 size, ImU32 fill, int child_flags, int window_flags, ImU32 edge)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(fill));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(edge ? edge : kStroke));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Dp(kRadiusLg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(8.0f), Dp(8.0f)));
    const int  flags = child_flags | (edge ? ImGuiChildFlags_Borders : 0) | ImGuiChildFlags_AlwaysUseWindowPadding;
    const bool shown = ImGui::BeginChild(id, size, flags, window_flags);
    ImGui::PopStyleVar(2);  // read when the child begins: the popups opened inside keep their own
    ImGui::PopStyleColor(2);
    return shown;
}

bool BeginPane(const char* id, ImVec2 size, const PaneOpts& o)
{
    const bool all = o.edge == PaneEdge::All;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(o.fill));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(o.edge_col));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Dp(o.radius));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(o.pad), Dp(o.pad)));
    const int  flags = o.child_flags | (all ? ImGuiChildFlags_Borders : 0) | ImGuiChildFlags_AlwaysUseWindowPadding;
    const bool shown = ImGui::BeginChild(id, size, flags, o.window_flags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (o.edge == PaneEdge::Left) {  // a stroke line on its left only (.edside's border-left)
        const ImVec2 wp = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(wp, ImVec2(wp.x, wp.y + ImGui::GetWindowSize().y), Ink(o.edge_col));
    }
    return shown;
}

void EndPane() { ImGui::EndChild(); }

bool BeginListPane(const char* id, ImVec2 size, int window_flags)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(kBg));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStroke));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Dp(kRadiusSm));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.0f, 1.0f));  // rows inside the 1 px edge
    const bool shown = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, window_flags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    return shown;
}

void EndListPane() { ImGui::EndChild(); }

namespace {

// Narrows the window's work and content rects to `inner` (a box's inner right edge), saving them.
void NarrowTo(float inner, float* saved_work, float* saved_content)
{
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    *saved_work = win->WorkRect.Max.x;
    *saved_content = win->ContentRegionRect.Max.x;
    win->WorkRect.Max.x = std::min(win->WorkRect.Max.x, inner);
    win->ContentRegionRect.Max.x = std::min(win->ContentRegionRect.Max.x, inner);
}
void RestoreFrom(float saved_work, float saved_content)
{
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    win->WorkRect.Max.x = saved_work;
    win->ContentRegionRect.Max.x = saved_content;
}

}  // namespace

void BeginWell(Well& k, float pad)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    k.p0 = ImGui::GetCursorScreenPos();
    k.w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    k.pad = pad >= 0.0f ? pad : Dp(4.0f) + 1.0f;  // the padding inside the 1 px edge
    k.split.Split(dl, 2);
    k.split.SetCurrentChannel(dl, 1);
    ImGui::SetCursorScreenPos(ImVec2(k.p0.x + k.pad, k.p0.y + k.pad));
    ImGui::BeginGroup();
    NarrowTo(std::max(k.p0.x + k.pad + 1.0f, k.p0.x + k.w - k.pad), &k.saved_work_max, &k.saved_content_max);
}

void EndWell(Well& k)
{
    RestoreFrom(k.saved_work_max, k.saved_content_max);
    ImGui::EndGroup();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bottom = ImGui::GetItemRectMax().y + k.pad;
    k.split.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(k.p0, ImVec2(k.p0.x + k.w, bottom), Ink(kBg), kRadiusMd);
    dl->AddRect(k.p0, ImVec2(k.p0.x + k.w, bottom), Ink(kStroke), kRadiusMd);
    k.split.Merge(dl);
    ImGui::SetCursorScreenPos(ImVec2(k.p0.x, bottom));
    // The pane's 8 px gap under it.
    ImGui::Dummy(ImVec2(k.w, std::max(0.0f, Dp(8.0f) - ImGui::GetStyle().ItemSpacing.y)));
}

void BeginToolbar(Well& k)
{
    BeginWell(k);
    if (g_toolbar_frame != ImGui::GetFrameCount()) {  // a new frame: no toolbar is open yet
        g_toolbar_frame = ImGui::GetFrameCount();
        g_toolbar_depth = 0;
    }
    // .tbb: 24 px buttons (pad 0 6), 3 px apart, wrapping inside the well.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Dp(6.0f), std::max(0.0f, (FieldH() - ImGui::GetFontSize()) * 0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Dp(3.0f), Dp(3.0f)));
    k.toolbar = true;
    if (g_toolbar_depth < static_cast<int>(sizeof(g_toolbar_win) / sizeof(g_toolbar_win[0])))
        g_toolbar_win[g_toolbar_depth] = ImGui::GetCurrentWindow();
    ++g_toolbar_depth;
}

void EndToolbar(Well& k)
{
    if (k.toolbar) {
        k.toolbar = false;
        g_toolbar_depth = std::max(0, g_toolbar_depth - 1);
        ImGui::PopStyleVar(2);
    }
    EndWell(k);
}

void BeginConfirmBand(ConfirmBand& b, float width, bool warn)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    b.p0 = ImGui::GetCursorScreenPos();
    b.w = std::max(1.0f, width > 0.0f ? width : ImGui::GetContentRegionAvail().x);
    b.warn = warn;
    b.pad_x = warn ? Dp(8.0f) : Dp(6.0f);
    b.pad_y = warn ? Dp(3.0f) : Dp(4.0f);
    b.split.Split(dl, 2);
    b.split.SetCurrentChannel(dl, 1);
    ImGui::SetCursorScreenPos(ImVec2(b.p0.x + b.pad_x, b.p0.y + b.pad_y));
    ImGui::BeginGroup();
    NarrowTo(std::max(b.p0.x + b.pad_x + 1.0f, b.p0.x + b.w - b.pad_x), &b.saved_work_max, &b.saved_content_max);
}

void EndConfirmBand(ConfirmBand& b)
{
    RestoreFrom(b.saved_work_max, b.saved_content_max);
    ImGui::EndGroup();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bottom = ImGui::GetItemRectMax().y + b.pad_y;
    const ImVec2 b1(b.p0.x + b.w, bottom);
    b.split.SetCurrentChannel(dl, 0);
    if (b.warn) {  // the Legacy band
        dl->AddRectFilled(b.p0, b1, Ink(WithAlpha(kWarn, 0x14)), kRadiusSm);
        dl->AddRect(b.p0, b1, Ink(kConfirmLine), kRadiusSm);
    } else {  // .confirmbox
        dl->AddRectFilled(b.p0, b1, Ink(WithAlpha(kConfirmLine, 0x14)), kRadiusMd);
        dl->AddRect(b.p0, b1, Ink(kConfirmLine), kRadiusMd);
    }
    b.split.Merge(dl);
    ImGui::SetCursorScreenPos(ImVec2(b.p0.x, bottom));
    ImGui::Dummy(ImVec2(b.w, 0.0f));
}

// ---- colours (13-14 G-12) -------------------------------------------------------------------

bool ColorSwatchButton(const char* id, float rgb[3], const char* tip)
{
    return ColorSwatchButton(id, rgb, tip, ColorSwatchOpts());
}

bool ColorSwatchButton(const char* id, float rgb[3], const char* tip, const ColorSwatchOpts& o)
{
    if (!rgb) return false;
    ImGui::PushID(id);
    const float  s = ControlH();
    const bool   clicked = ImGui::InvisibleButton("##swatch", ImVec2(s, s));
    const bool   hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  in = Dp(3.0f);
    dl->AddRectFilled(a, b, Ink(kBg), kRadiusSm);
    dl->AddRectFilled(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in),
                      Ink(ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f))), Dp(3.0f));
    dl->AddRect(a, b, Ink(hovered ? kFrameHover : kStrokeStrong), kRadiusSm);
    TipIfHovered(tip);
    // 13-14 (finishing): the colour when the picker opened (its Original preview), kept with the swatch.
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID ref_id = ImGui::GetID("##ref");
    if (clicked) {
        ImGui::OpenPopup("##swatchpick");
        for (int i = 0; i < 3; ++i) st->SetFloat(ref_id + static_cast<ImGuiID>(i), rgb[i]);
    }
    bool changed = false;
    if (BeginPanelPopup("##swatchpick")) {
        ImGui::SetNextItemWidth(Dp(180.0f));
        int flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_PickerHueBar;
        if (o.inputs) flags |= ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_DisplayHex;
        else flags |= ImGuiColorEditFlags_NoInputs;
        if (!o.original) flags |= ImGuiColorEditFlags_NoSidePreview;
        float ref[4] = {st->GetFloat(ref_id, rgb[0]), st->GetFloat(ref_id + 1, rgb[1]), st->GetFloat(ref_id + 2, rgb[2]), 1.0f};
        float col[4] = {rgb[0], rgb[1], rgb[2], 1.0f};
        changed = ImGui::ColorPicker4("##pick", col, flags, o.original ? ref : nullptr);
        if (changed)
            for (int i = 0; i < 3; ++i) rgb[i] = col[i];
        EndPanelPopup();
    }
    ImGui::PopID();
    return changed;
}

bool SwatchPalette(const char* id, const ImU32* colors, int count, ImU32 current, int* picked)
{
    if (!colors || count <= 0) return false;
    ImGui::PushID(id);
    bool        clicked_any = false;
    const float cell = Dp(20.0f);
    const float gap = Dp(4.0f);
    for (int i = 0; i < count; ++i) {
        // Width-aware (ui_flow.h): beside the previous dot when it fits, else on the next row.
        if (i > 0 && (WindowAutoSizes() || flow::FitsAfter(ImGui::GetItemRectMax().x, gap, cell, LineRight())))
            ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const bool   clicked = ImGui::InvisibleButton("##col", ImVec2(cell, cell));
        const bool   hovered = ImGui::IsItemHovered();
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        const ImVec2 c(q.x + cell * 0.5f, q.y + cell * 0.5f);
        dl->AddCircleFilled(c, Dp(7.0f), Ink(colors[i]), 16);
        if ((colors[i] | IM_COL32_A_MASK) == (current | IM_COL32_A_MASK)) dl->AddCircle(c, Dp(9.0f), Ink(kText), 16, Dp(1.5f));
        else if (hovered) dl->AddCircle(c, Dp(9.0f), Ink(kStrokeStrong), 16, Dp(1.5f));
        if (clicked) {
            clicked_any = true;
            if (picked) *picked = i;
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    return clicked_any;
}

void PushPanelStyle()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(kSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStroke));
}

void PushOverlayPanelStyle()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(kOverlayBg));
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

namespace {

// 13-14 (finishing): SectionOpts.fill_h -- the open section's body, a child as high as the room left
// less the card's bottom pad and its gap under it (.secx.sg { flex: 1 }), scrolling. Once.
void BeginSectionBody(Section& s)
{
    if (!s.fill_h || !s.open || s.body) return;
    const float sp_y = ImGui::GetStyle().ItemSpacing.y;
    const float body_h =
        std::max(Dp(s.fill_min_h), ImGui::GetContentRegionAvail().y - kCardPad - std::max(0.0f, kCardGap - sp_y) - 1.0f);
    ImGui::BeginChild("##body", ImVec2(0.0f, body_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    s.body = true;
}

}  // namespace

bool BeginSection(Section& s, const char* title, const SectionOpts& o)
{
    ImGui::PushID(title);
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID("##open");
    bool open = !o.foldable || store->GetBool(open_id, true);
    if (o.foldable) NoteFoldableSection(open_id, &open);  // 13-33: Expand / Collapse all of its scope
    s.nested = g_section_depth > 0;
    ++g_section_depth;
    s.right_w = std::max(0.0f, o.right_w);
    s.in_header = false;
    s.fill_h = o.fill_h;
    s.fill_min_h = o.fill_min_h;
    s.body = false;

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
    if (s.right_w <= 0.0f) BeginSectionBody(s);
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
    BeginSectionBody(s);
}

void EndSection(Section& s)
{
    if (s.in_header) EndSectionHeaderRight(s);
    if (s.body) {
        ImGui::EndChild();
        s.body = false;
    }
    EndCard(s.card, s.nested ? kBg : kSurface, kStroke);  // .secx .secx: a section inside a section is kBg
    g_section_depth = std::max(0, g_section_depth - 1);
    ImGui::PopID();
}

namespace {
float SentenceGap() { return ImGui::GetStyle().ItemSpacing.x; }
}  // namespace

float SentenceColumnWidth(const char* const* labels, int count, const char* badge)
{
    float w = badge ? ImGui::CalcTextSize(badge).x + Dp(8.0f) : 0.0f;
    for (int i = 0; labels && i < count; ++i)
        if (labels[i]) w = std::max(w, ImGui::CalcTextSize(labels[i]).x);
    return w;
}

Sentence MakeSentence(float x0, float right, float column_w)
{
    Sentence s;
    s.x0 = x0;
    s.chips_x = x0 + column_w + SentenceGap();
    s.right = std::max(s.chips_x + Dp(20.0f), right);
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
        const float bx = xr - ts.x - Dp(8.0f);
        dl->AddRectFilled(ImVec2(bx, y + Dp(3.0f)), ImVec2(xr, y + fh - Dp(3.0f)), kBg, Dp(4.0f));
        dl->AddRect(ImVec2(bx, y + Dp(3.0f)), ImVec2(xr, y + fh - Dp(3.0f)), kStroke, Dp(4.0f));
        dl->AddText(ImVec2(bx + Dp(4.0f), y + (fh - ts.y) * 0.5f), kMuted, label);
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
    const float h = ImGui::GetTextLineHeight() + Dp(10.0f);
    const float pad_x = Dp(12.0f);  // the text's inset (the accent bar sits in it)
    float w = width;
    if (w <= 0.0f) {
        for (int i = 0; i < count; ++i)
            w = std::max(w, ImGui::CalcTextSize(labels[i], nullptr, true).x + 2.0f * pad_x);
    }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const ImVec2 a(p0.x, p0.y + static_cast<float>(i) * (h + Dp(2.0f)));
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
            dl->AddRectFilled(ImVec2(a.x, a.y + Dp(4.0f)), ImVec2(a.x + Dp(3.0f), b.y - Dp(4.0f)), kAccent, 1.5f);
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
    ImGui::Dummy(ImVec2(w, static_cast<float>(count) * (h + Dp(2.0f)) - Dp(2.0f)));
    ImGui::PopID();
    return changed;
}

}  // namespace ui
}  // namespace rav

#endif  // _WIN32
