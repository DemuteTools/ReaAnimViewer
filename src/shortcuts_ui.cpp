// SPDX-License-Identifier: MIT
//
// See shortcuts_ui.h. The popup lists every key action (double-click = record a new key,
// right-click = back to the default) and every mouse gesture (fixed, greyed).

#include "shortcuts_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <string>

#include "shortcuts.h"
#include "ui_theme.h"

namespace rav {
namespace {

constexpr char kPopupId[]  = "##shortcuts";
constexpr char kRecordId[] = "##recordkey";

// Mockup px (Dp at use).
constexpr float kIconSize = 18.0f;   // the glyph
constexpr float kIconPad  = 3.0f;    // around it: a 24 px button
constexpr float kRowW     = 350.0f;  // the popup's rows
constexpr float kRowH     = 26.0f;

bool g_open          = false;  // the popup was open last frame
bool g_open_at_press = false;  // ...when the icon was pressed (a second click closes it)
bool g_close_request = false;  // Esc from the viewer's key handling

// The fallback glyph when the icon texture is missing: a keyboard outline, keys, space bar.
void DrawKeyboardGlyph(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    const float w = s * 0.84f, h = s * 0.56f;
    const ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
    dl->AddRect(a, b, col, ui::Dp(2.0f), 0, ui::Dp(1.5f));
    const float k = w / 7.0f;
    for (int row = 0; row < 2; ++row)
        for (int i = 0; i < 5; ++i) {
            const ImVec2 p(a.x + k * (1.0f + static_cast<float>(i)), a.y + h * (0.22f + 0.24f * static_cast<float>(row)));
            dl->AddRectFilled(p, ImVec2(p.x + k * 0.6f, p.y + k * 0.6f), col);
        }
    dl->AddRectFilled(ImVec2(c.x - k * 1.5f, b.y - h * 0.3f), ImVec2(c.x + k * 1.5f, b.y - h * 0.3f + k * 0.55f), col);
}

void Header(const char* title, const char* note)
{
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    ui::GroupCaption(title);
    if (note) {
        ImGui::SameLine();
        ui::ToneText(note, ui::kFaint);
    }
}

// The key column: caps right-aligned here, "fixed" right of them.
float CapRight(float row_right)
{
    return row_right - ui::Dp(10.0f) - ImGui::CalcTextSize("fixed").x - ui::Dp(10.0f);
}

// One key action. Returns true when a double-click asked to record it.
bool KeyRow(int id)
{
    const ShortcutDef& def = kShortcutTable[id];
    const bool recording = (ShortcutRecordingId() == id);
    const bool custom = CurrentShortcuts()[static_cast<size_t>(id)] != def.def;
    ImGui::PushID(id);
    ui::SelectRow("##key", recording, 0, ui::Dp(kRowH),
                  ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_NoAutoClosePopups);
    bool record = false;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) record = true;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ResetShortcutToDefault(id);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // The row's hover (kRaised) would not show on the popup's kRaised: kHover instead.
    if (!recording && ui::SelectRowHovered()) dl->AddRectFilled(a, b, ui::kHover, ui::kRadiusSm);
    const float th = ImGui::GetTextLineHeight();
    const float cy = (a.y + b.y) * 0.5f;
    const char* key = ShortcutKeyLabel(id);
    const float cap_w = ImGui::CalcTextSize(key).x + ui::Dp(8.0f);
    const float cap_x = CapRight(b.x) - cap_w;
    // The label gives way to a long key ("Ctrl+Shift+Alt+PageDown").
    dl->PushClipRect(a, ImVec2(std::max(a.x, cap_x - ui::Dp(6.0f)), b.y), true);
    dl->AddText(ImVec2(a.x + ui::Dp(8.0f), cy - th * 0.5f), ui::kText, def.label);
    if (def.context != ShortcutContext::Anywhere) {
        const float lw = ImGui::CalcTextSize(def.label).x;
        dl->AddText(ImVec2(a.x + ui::Dp(8.0f) + lw + ui::Dp(6.0f), cy - th * 0.5f), ui::kFaint,
                    def.context == ShortcutContext::VideoView ? "Video" : "Tagging");
    }
    dl->PopClipRect();
    // A custom key shows in the accent colour.
    ui::KeyCap(dl, ImVec2(cap_x, cy - th * 0.5f - ui::Dp(1.5f)), key, custom ? ui::kAccent : ui::kText);
    ImGui::PopID();
    return record;
}

// One mouse gesture: read-only, greyed, "fixed".
void GestureRow(const char* action, const char* gesture)
{
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(ui::Dp(kRowW), ui::Dp(kRowH)));
    const float th = ImGui::GetTextLineHeight();
    const float cy = a.y + ui::Dp(kRowH) * 0.5f;
    const float right = a.x + ui::Dp(kRowW);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(a.x + ui::Dp(8.0f), cy - th * 0.5f), ui::kMuted, action);
    const float cap_w = ImGui::CalcTextSize(gesture).x + ui::Dp(8.0f);
    ui::KeyCap(dl, ImVec2(CapRight(right) - cap_w, cy - th * 0.5f - ui::Dp(1.5f)), gesture, ui::kMuted);
    const float fw = ImGui::CalcTextSize("fixed").x;
    dl->AddText(ImVec2(right - ui::Dp(10.0f) - fw, cy - th * 0.5f), ui::kFaint, "fixed");
}

void Note(const char* text)
{
    ui::ToneText(text, ui::kFaint, /*wrap=*/true, ui::Dp(kRowW));
}

void RecordModal(bool open_now)
{
    if (open_now) ImGui::OpenPopup(kRecordId);
    if (ui::BeginModalCard(kRecordId)) {
        const int id = ShortcutRecordingId();
        if (id < 0) {
            ImGui::CloseCurrentPopup();  // a key was taken, or Esc
        } else {
            ImGui::Text("Press a key for %s...", kShortcutTable[id].label);
            ui::Caption("Ctrl, Shift and Alt can be held with it. Esc to cancel.");
            if (const char* owner = ShortcutRecordingConflict()) {
                ui::ToneText(std::string("Already used by ") + owner, ui::kWarn);
            } else {
                ImGui::TextUnformatted(" ");  // keeps the modal's size when the warning shows
            }
            ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
            if (ui::SolidButton("Cancel", ImVec2(ui::Dp(90.0f), 0.0f))) {
                CancelShortcutRecording();
                ImGui::CloseCurrentPopup();
            }
        }
        ui::EndModalCard();
    }
}

void PopupBody()
{
    ImGui::TextUnformatted("Shortcuts");
    Note("Double-click a key to change it, right-click it to restore its default. "
         "Keys work while the viewer has the focus.");

    Header("KEYS", nullptr);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Dp(8.0f), ui::Dp(2.0f)));
    int record = -1;
    for (int i = 0; i < kShortcutCount; ++i)
        if (KeyRow(i)) record = i;
    ImGui::PopStyleVar();
    if (record >= 0) BeginShortcutRecording(record);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Dp(8.0f), ui::Dp(2.0f)));
    constexpr int kGroups = static_cast<int>(sizeof(kMouseGestureGroups) / sizeof(kMouseGestureGroups[0]));
    for (int g = 0; g < kGroups; ++g) {
        const MouseGestureGroup& grp = kMouseGestureGroups[g];
        Header(grp.title, grp.note);
        for (const MouseGestureDef& m : kMouseGestureTable)
            if (m.group == g) GestureRow(m.action, m.gesture);
        if (grp.footer) Note(grp.footer);
    }
    ImGui::PopStyleVar();

    RecordModal(record >= 0);
}

}  // namespace

void DrawShortcutsButton(float right_x, float centre_y, ImTextureID icon)
{
    const float btn = ui::Dp(kIconSize + 2.0f * kIconPad);
    const bool  shown = ui::BeginOverlay("##keysbtn", ImVec2(right_x, centre_y - btn * 0.5f), ImVec2(1.0f, 0.0f));
    if (shown) {
        // Frameless like the menu button; lit while the popup is open. No tooltip: the
        // popup is the help.
        ui::IconOnlyOpts o;
        o.w = btn;
        o.on = g_open;
        o.icon_px = ui::Dp(kIconSize);
        const bool pressed = ui::IconOnlyButton("##keys", icon, nullptr, o);
        if (!icon) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const ImU32  tint = (g_open || ImGui::IsItemHovered()) ? ui::kText : ui::kMuted;
            DrawKeyboardGlyph(ImGui::GetWindowDrawList(), ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f),
                              ui::Dp(kIconSize), tint);
        }
        // A click outside the popup closes it on the press; a press on the icon while it
        // was open must not reopen it on the release.
        if (ImGui::IsItemActivated()) g_open_at_press = g_open;
        if (pressed && !g_open_at_press) ImGui::OpenPopup(kPopupId);
        const ImVec2 bmax = ImGui::GetItemRectMax();

        ImGui::SetNextWindowPos(ImVec2(bmax.x, bmax.y + ui::Dp(6.0f)), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        const bool open = ui::BeginPanelPopup(kPopupId);
        if (open) {
            if (g_close_request && ShortcutRecordingId() < 0) ImGui::CloseCurrentPopup();
            PopupBody();
            ui::EndPanelPopup();
        }
        g_open = open;
    } else {
        g_open = false;
    }
    g_close_request = false;
    ui::EndOverlay();

    // The popup went away (e.g. the viewer closed it): no recording without its modal.
    if (!g_open && ShortcutRecordingId() >= 0) CancelShortcutRecording();
}

bool ShortcutsPopupOpen()
{
    return g_open;
}

void RequestCloseShortcutsPopup()
{
    g_close_request = true;
}

}  // namespace rav

#endif  // _WIN32
