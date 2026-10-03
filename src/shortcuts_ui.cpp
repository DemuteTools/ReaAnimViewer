// SPDX-License-Identifier: MIT
//
// See shortcuts_ui.h. The popup lists every key action (double-click = record a new key,
// right-click = back to the default) and every mouse gesture (fixed, greyed).

#include "shortcuts_ui.h"

#ifdef _WIN32

#include <algorithm>

#include "shortcuts.h"
#include "ui_theme.h"

namespace rav {
namespace {

constexpr char kPopupId[]  = "##shortcuts";
constexpr char kRecordId[] = "##recordkey";

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
    dl->AddRect(a, b, col, 2.0f, 0, 1.5f);
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
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ui::Caption(title);
    if (note) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kFaint));
        ImGui::TextUnformatted(note);
        ImGui::PopStyleColor();
    }
}

// The key column: caps right-aligned here, "fixed" right of them.
float CapRight(float row_right)
{
    return row_right - 10.0f - ImGui::CalcTextSize("fixed").x - 10.0f;
}

// One key action. Returns true when a double-click asked to record it.
bool KeyRow(int id)
{
    const ShortcutDef& def = kShortcutTable[id];
    const bool recording = (ShortcutRecordingId() == id);
    const bool custom = CurrentShortcuts()[static_cast<size_t>(id)] != def.def;
    ImGui::PushID(id);
    ImGui::Selectable("##key", recording,
                      ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_NoAutoClosePopups,
                      ImVec2(kRowW, kRowH));
    bool record = false;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) record = true;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ResetShortcutToDefault(id);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float th = ImGui::GetTextLineHeight();
    const float cy = (a.y + b.y) * 0.5f;
    const char* key = ShortcutKeyLabel(id);
    const float cap_w = ImGui::CalcTextSize(key).x + 8.0f;
    const float cap_x = CapRight(b.x) - cap_w;
    // The label gives way to a long key ("Ctrl+Shift+Alt+PageDown").
    dl->PushClipRect(a, ImVec2(std::max(a.x, cap_x - 6.0f), b.y), true);
    dl->AddText(ImVec2(a.x + 8.0f, cy - th * 0.5f), ui::kText, def.label);
    if (def.context == ShortcutContext::VideoView) {
        const float lw = ImGui::CalcTextSize(def.label).x;
        dl->AddText(ImVec2(a.x + 8.0f + lw + 6.0f, cy - th * 0.5f), ui::kFaint, "Video view");
    }
    dl->PopClipRect();
    // A custom key shows in the accent colour.
    ui::KeyCap(dl, ImVec2(cap_x, cy - th * 0.5f - 1.5f), key, custom ? ui::kAccent : ui::kText);
    ImGui::PopID();
    return record;
}

// One mouse gesture: read-only, greyed, "fixed".
void GestureRow(const char* action, const char* gesture)
{
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(kRowW, kRowH));
    const float th = ImGui::GetTextLineHeight();
    const float cy = a.y + kRowH * 0.5f;
    const float right = a.x + kRowW;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(a.x + 8.0f, cy - th * 0.5f), ui::kMuted, action);
    const float cap_w = ImGui::CalcTextSize(gesture).x + 8.0f;
    ui::KeyCap(dl, ImVec2(CapRight(right) - cap_w, cy - th * 0.5f - 1.5f), gesture, ui::kMuted);
    const float fw = ImGui::CalcTextSize("fixed").x;
    dl->AddText(ImVec2(right - 10.0f - fw, cy - th * 0.5f), ui::kFaint, "fixed");
}

void Note(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kFaint));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kRowW);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void RecordModal(bool open_now)
{
    if (open_now) ImGui::OpenPopup(kRecordId);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(kRecordId, nullptr, kFlags)) {
        const int id = ShortcutRecordingId();
        if (id < 0) {
            ImGui::CloseCurrentPopup();  // a key was taken, or Esc
        } else {
            ImGui::Text("Press a key for %s...", kShortcutTable[id].label);
            ui::Caption("Ctrl, Shift and Alt can be held with it. Esc to cancel.");
            if (const char* owner = ShortcutRecordingConflict()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kWarn));
                ImGui::Text("Already used by %s", owner);
                ImGui::PopStyleColor();
            } else {
                ImGui::TextUnformatted(" ");  // keeps the modal's size when the warning shows
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            if (ui::SolidButton("Cancel", ImVec2(90.0f, 0.0f))) {
                CancelShortcutRecording();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void PopupBody()
{
    ImGui::TextUnformatted("Shortcuts");
    Note("Double-click a key to change it, right-click it to restore its default. "
         "Keys work while the viewer has the focus.");

    Header("KEYS", nullptr);
    ImGui::PushStyleColor(ImGuiCol_Header, ui::Col(ui::kAccentSoft));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui::Col(ui::kHover));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ui::Col(ui::kAccentSoft));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));
    int record = -1;
    for (int i = 0; i < kShortcutCount; ++i)
        if (KeyRow(i)) record = i;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (record >= 0) BeginShortcutRecording(record);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));
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
    const float btn = kIconSize + 2.0f * kIconPad;
    ImGui::SetNextWindowPos(ImVec2(right_x, centre_y - btn * 0.5f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kIconPad, kIconPad));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBackground;
    const bool shown = ImGui::Begin("##keysbtn", nullptr, kFlags);
    ImGui::PopStyleVar(3);
    if (shown) {
        // Frameless like the menu button; lit while the popup is open. No tooltip: the
        // popup is the help.
        ImGui::PushStyleColor(ImGuiCol_Button, g_open ? ui::Col(ui::kAccentSoft) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.16f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kIconPad, kIconPad));
        const ImU32 tint = g_open ? ui::kText : ui::kMuted;
        bool pressed;
        if (icon) {
            pressed = ImGui::ImageButton("##keys", icon, ImVec2(kIconSize, kIconSize), ImVec2(0, 0), ImVec2(1, 1),
                                         ImVec4(0, 0, 0, 0), ui::Col(tint));
        } else {
            pressed = ImGui::Button("##keys", ImVec2(btn, btn));
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            DrawKeyboardGlyph(ImGui::GetWindowDrawList(), ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), kIconSize,
                              tint);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        // A click outside the popup closes it on the press; a press on the icon while it
        // was open must not reopen it on the release.
        if (ImGui::IsItemActivated()) g_open_at_press = g_open;
        if (pressed && !g_open_at_press) ImGui::OpenPopup(kPopupId);
        const ImVec2 bmax = ImGui::GetItemRectMax();

        ImGui::SetNextWindowPos(ImVec2(bmax.x, bmax.y + 6.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ui::Col(ui::kRaised));
        ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStrokeStrong));
        const bool open = ImGui::BeginPopup(kPopupId);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        if (open) {
            if (g_close_request && ShortcutRecordingId() < 0) ImGui::CloseCurrentPopup();
            PopupBody();
            ImGui::EndPopup();
        }
        g_open = open;
    } else {
        g_open = false;
    }
    g_close_request = false;
    ImGui::End();

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
