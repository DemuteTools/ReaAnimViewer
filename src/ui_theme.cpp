// SPDX-License-Identifier: MIT
//
// See ui_theme.h.

#include "ui_theme.h"

#ifdef _WIN32

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

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

namespace {

// One DragNumber at a time is dragged or typed into.
struct DragNumberState {
    ImGuiID drag = 0;          // the field being dragged
    double  start = 0.0;       // its value when the press began
    double  acc = 0.0;         // the drag's change so far
    float   press_x = 0.0f;
    bool    moved = false;     // past the drag threshold: a drag, not a click
    bool    cancelled = false; // Esc during the drag: inert until the release
    ImGuiID typing = 0;        // the field being typed into
    int     focus_frames = 0;  // frames left for the text field to take the focus
    char    buf[64] = {};
    int     seen_frame = -1;   // the frame the dragged / typed field was last drawn
};
DragNumberState g_dn;

double RoundTo(double v, int decimals)
{
    const double k = std::pow(10.0, decimals < 0 ? 0 : (decimals > 9 ? 9 : decimals));
    const double r = std::round(v * k) / k;
    return r == 0.0 ? 0.0 : r;  // never -0
}

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
    g_dn.cancelled = false;
}

DragNumberEvent DragNumber(const char* id, double* value, double step, int decimals, const char* unit, float width)
{
    DragNumberEvent ev = DragNumberEvent::None;
    if (!value) return ev;
    ImGui::PushID(id);
    const ImGuiID gid = ImGui::GetID("##dragnum");
    const ImGuiIO& io = ImGui::GetIO();
    const float h = ImGui::GetFrameHeight();
    const float w = width > 0.0f ? width : ImGui::CalcTextSize("-0000.00").x + 12.0f;
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", decimals < 0 ? 0 : decimals, RoundTo(*value, decimals));

    if (g_dn.typing == gid || g_dn.drag == gid) g_dn.seen_frame = ImGui::GetFrameCount();
    if (g_dn.typing == gid) {
        ImGui::SetNextItemWidth(w);
        if (g_dn.focus_frames > 0) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##type", g_dn.buf, sizeof(g_dn.buf),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool active = ImGui::IsItemActive();
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
            g_dn.start = *value;
            g_dn.acc = 0.0;
            g_dn.press_x = io.MousePos.x;
            g_dn.moved = false;
            g_dn.cancelled = false;
            g_dn.seen_frame = ImGui::GetFrameCount();
        }
        if (active && g_dn.drag == gid && !g_dn.cancelled) {
            const double k = step * (io.KeyShift ? 0.1 : 1.0);
            if (!g_dn.moved && std::fabs(io.MousePos.x - g_dn.press_x) >= io.MouseDragThreshold) {
                g_dn.moved = true;
                g_dn.acc = static_cast<double>(io.MousePos.x - g_dn.press_x) * k;
            } else if (g_dn.moved) {
                g_dn.acc += static_cast<double>(io.MouseDelta.x) * k;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                g_dn.cancelled = true;
                if (*value != g_dn.start) {
                    *value = g_dn.start;
                }
                ev = DragNumberEvent::Cancel;
            } else if (g_dn.moved) {
                const double nv = RoundTo(g_dn.start + g_dn.acc, decimals);
                if (nv != *value) {
                    *value = nv;
                    ev = DragNumberEvent::Live;
                }
            }
        }
        if (ImGui::IsItemDeactivated() && g_dn.drag == gid) {
            if (!g_dn.cancelled) {
                if (g_dn.moved) {
                    ev = DragNumberEvent::Commit;
                } else {  // a click: type a value
                    g_dn.typing = gid;
                    g_dn.focus_frames = 3;
                    g_dn.seen_frame = ImGui::GetFrameCount();
                    std::snprintf(g_dn.buf, sizeof(g_dn.buf), "%s", text);
                }
            }
            g_dn.drag = 0;
        }
        if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p1(p0.x + w, p0.y + h);
        dl->AddRectFilled(p0, p1, kBg, kRadiusSm);
        dl->AddRect(p0, p1, (hovered || active) ? kStrokeStrong : kStroke, kRadiusSm);
        std::snprintf(text, sizeof(text), "%.*f", decimals < 0 ? 0 : decimals, RoundTo(*value, decimals));
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->PushClipRect(p0, p1, true);
        dl->AddText(ImVec2(p0.x + 6.0f, p0.y + (h - ts.y) * 0.5f), kText, text);
        dl->PopClipRect();
    }
    if (unit && unit[0]) {
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
        ImGui::TextUnformatted(unit);
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return ev;
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
    const float avail = max_x - p.x;
    if (ImGui::CalcTextSize(text.c_str()).x <= avail) {
        dl->AddText(p, col, text.c_str());
        return false;
    }
    const float dots = ImGui::CalcTextSize("...").x;
    size_t n = text.size();
    while (n > 0) {
        do {
            --n;
        } while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80);  // a whole UTF-8 glyph
        if (ImGui::CalcTextSize(text.c_str(), text.c_str() + n).x + dots <= avail) break;
    }
    std::string s = text.substr(0, n);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    s += "...";
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

void BeginCard(Card& k)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    k.p0 = ImGui::GetCursorScreenPos();
    k.w = ImGui::GetContentRegionAvail().x;
    k.split.Split(dl, 2);
    k.split.SetCurrentChannel(dl, 1);
    ImGui::SetCursorScreenPos(ImVec2(k.p0.x + kCardPad, k.p0.y + kCardPad));
    ImGui::BeginGroup();
}

void EndCard(Card& k, ImU32 fill, ImU32 edge)
{
    ImGui::EndGroup();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bottom = ImGui::GetItemRectMax().y + kCardPad;
    k.split.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(k.p0, ImVec2(k.p0.x + k.w, bottom), fill, kRadiusMd);
    dl->AddRect(k.p0, ImVec2(k.p0.x + k.w, bottom), edge, kRadiusMd);
    k.split.Merge(dl);
    ImGui::SetCursorScreenPos(ImVec2(k.p0.x, bottom));
    ImGui::Dummy(ImVec2(k.w, std::max(0.0f, kCardGap - ImGui::GetStyle().ItemSpacing.y)));
}

void PushPanelStyle()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(kSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kStroke));
}

bool BeginSection(Section& s, const char* title, ImTextureID icon, bool foldable)
{
    ImGui::PushID(title);
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID("##open");
    bool open = !foldable || store->GetBool(open_id, true);

    BeginCard(s.card);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float  fh = ImGui::GetFrameHeight();
    const float  w = std::max(1.0f, s.card.w - 2.0f * kCardPad);
    const ImVec2 p = ImGui::GetCursorScreenPos();

    // The header: a button over the whole row when the section folds.
    bool hovered = false;
    if (foldable) {
        if (ImGui::InvisibleButton("##head", ImVec2(w, fh))) {
            open = !open;
            store->SetBool(open_id, open);
        }
        hovered = ImGui::IsItemHovered();
    } else {
        ImGui::Dummy(ImVec2(w, fh));
    }
    if (hovered)
        dl->AddRectFilled(ImVec2(p.x - 4.0f, p.y), ImVec2(p.x + w + 4.0f, p.y + fh), kHover, kRadiusSm);
    float tx = p.x;
    if (icon) {
        const float is = 16.0f;
        dl->AddImage(icon, ImVec2(p.x, p.y + (fh - is) * 0.5f), ImVec2(p.x + is, p.y + (fh + is) * 0.5f),
                     ImVec2(0, 0), ImVec2(1, 1), IM_COL32(0xD1, 0xD6, 0xE5, 0xFF));
        tx += is + 8.0f;
    }
    const ImVec2 ts = ImGui::CalcTextSize(title, nullptr, true);
    dl->AddText(ImVec2(tx, p.y + (fh - ts.y) * 0.5f), kText, title, std::strstr(title, "##"));
    if (foldable) {
        // The chevron: down when open, right when folded.
        const ImVec2 c(p.x + w - 6.0f, p.y + fh * 0.5f);
        const float  r = 4.0f;
        const ImU32  col = hovered ? kText : kMuted;
        if (open)
            dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.5f), ImVec2(c.x + r, c.y - r * 0.5f),
                                  ImVec2(c.x, c.y + r * 0.6f), col);
        else
            dl->AddTriangleFilled(ImVec2(c.x - r * 0.5f, c.y - r), ImVec2(c.x - r * 0.5f, c.y + r),
                                  ImVec2(c.x + r * 0.6f, c.y), col);
    }
    if (open) {
        // The divider between the header and the settings, across the card.
        const float y = p.y + fh + 5.0f;
        dl->AddLine(ImVec2(s.card.p0.x + 1.0f, y), ImVec2(s.card.p0.x + s.card.w - 1.0f, y), kStroke, 1.0f);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }
    return open;
}

void EndSection(Section& s)
{
    EndCard(s.card, kSurface, kStroke);
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
    return field_w + ((unit && unit[0]) ? 4.0f + ImGui::CalcTextSize(unit).x : 0.0f);
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
