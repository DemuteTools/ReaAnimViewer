// SPDX-License-Identifier: MIT
//
// See shortcuts_ui.h. The popup lists every key action (double-click = record a new key,
// right-click = back to the default), 13-45's "View: scroll and zoom" rows (REAPER | Own) and every
// mouse gesture (fixed, greyed).

#include "shortcuts_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "shortcuts.h"
#include "ui_theme.h"
#include "view_nav.h"  // 13-45: the "View: scroll and zoom" rows

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
int  g_reset_refused = -1;     // 10-11 review: the action whose restore was refused (or -1)
int  g_reset_blocker = -1;     // ...the action that holds its default as your own key

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
        // The note (body font) on the caption's baseline (11 px small font).
        ImGui::SameLine();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + ui::FontSmall()->Ascent - ImGui::GetFont()->Ascent));
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
    // On the popup's kRaised the row's hover is kHover (kRowHoverStrong).
    ui::SelectRow("##key", recording, ui::kRowHoverStrong, ui::Dp(kRowH),
                  ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_NoAutoClosePopups);
    bool record = false;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) record = true;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        // 10-11 review: an action without a key cannot take back a default that is your own
        // key for another action: refused, and the popup says why.
        const int blocker = ResetShortcutToDefault(id);
        g_reset_refused = blocker >= 0 ? id : -1;
        g_reset_blocker = blocker;
    }
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float th = ImGui::GetTextLineHeight();
    const float cy = (a.y + b.y) * 0.5f;
    const char* key = ShortcutKeyLabel(id);
    if (!key[0]) key = "None";  // 10-11: unbound (its default is another action's key)
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

// 13-45 -- one "View: scroll and zoom" row (the mockup's option A): the label and the gestures that
// run it now on the first line; under them the REAPER | Own switch and what the row follows (the REAPER
// action and its id, or "RAV only" and what REAPER binds). Double-click = record an own gesture,
// right-click = linked to REAPER again. Returns true when a double-click asked to record.
bool NavRow(int row)
{
    const ViewNavDef&    def = kViewNavTable[row];
    const ViewNavChoice  ch = CurrentViewNavChoices()[static_cast<size_t>(row)];
    const bool           recording = ViewNavRecordingRow() == row;
    const float          line_h = ui::Dp(kRowH);
    const float          sub_h = ui::Dp(18.0f);
    ImGui::PushID(1000 + row);
    ImGui::SetNextItemAllowOverlap();  // the switch's two buttons sit on the row
    ui::SelectRow("##nav", recording, ui::kRowHoverStrong, line_h + sub_h,
                  ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_NoAutoClosePopups | ImGuiSelectableFlags_AllowOverlap);
    bool record = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) SetViewNavLinked(row);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  th = ImGui::GetTextLineHeight();
    const float  cy = a.y + line_h * 0.5f;

    // The gestures, right-aligned; as many as leave the label some room, then "+N".
    std::vector<std::string> caps;
    for (const ViewGesture& g : ViewNavGestures(CurrentViewNav(), row)) caps.push_back(ViewGestureLabel(g));
    ImU32 cap_col = ch.own ? ui::kAccent : ui::kText;
    if (caps.empty()) {
        caps.push_back(ch.own ? "double-click to set" : "not bound in REAPER");
        cap_col = ui::kFaint;
    }
    const float gap = ui::Dp(4.0f);
    const float right = b.x - ui::Dp(8.0f);
    const float room = right - (a.x + ui::Dp(8.0f) + std::min(ImGui::CalcTextSize(def.label).x, ui::Dp(90.0f)) + ui::Dp(6.0f));
    auto cap_w = [](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x + ui::Dp(8.0f); };
    size_t shown = caps.size();
    char   more[16] = "";
    for (;;) {
        float w = 0.0f;
        for (size_t i = 0; i < shown; ++i) w += cap_w(caps[i]) + (i ? gap : 0.0f);
        if (shown < caps.size()) {
            std::snprintf(more, sizeof(more), "+%d", static_cast<int>(caps.size() - shown));
            w += gap + cap_w(more);
        }
        if (w <= room || shown <= 1) break;
        --shown;
    }
    float total = 0.0f;
    for (size_t i = 0; i < shown; ++i) total += cap_w(caps[i]) + (i ? gap : 0.0f);
    if (shown < caps.size()) total += gap + cap_w(more);
    const float caps_x = std::max(a.x + ui::Dp(8.0f), right - total);
    // The label gives way to the gestures.
    dl->PushClipRect(a, ImVec2(std::max(a.x, caps_x - ui::Dp(6.0f)), b.y), true);
    dl->AddText(ImVec2(a.x + ui::Dp(8.0f), cy - th * 0.5f), ui::kText, def.label);
    dl->PopClipRect();
    {
        float x = caps_x;
        dl->PushClipRect(ImVec2(caps_x, a.y), ImVec2(b.x, b.y), true);
        for (size_t i = 0; i < shown; ++i) x += ui::KeyCap(dl, ImVec2(x, cy - th * 0.5f - ui::Dp(1.5f)), caps[i].c_str(), cap_col) + gap;
        if (shown < caps.size()) ui::KeyCap(dl, ImVec2(x, cy - th * 0.5f - ui::Dp(1.5f)), more, ui::kMuted);
        dl->PopClipRect();
    }
    if (shown < caps.size() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        std::string all;
        for (const std::string& c : caps) all += (all.empty() ? "" : ", ") + c;
        ui::ItemTipText(all.c_str());
    }

    // Second line: the REAPER | Own switch (a pill of two small buttons), then what the row follows.
    ImFont*     sf = ui::FontSmall();
    const float seg_h = ui::Dp(16.0f);
    const float sy = a.y + line_h + std::floor((sub_h - seg_h) * 0.5f) - ui::Dp(3.0f);
    float       sx = a.x + ui::Dp(8.0f);
    const char* const segs[2] = {"REAPER", "Own"};
    float       seg_w[2];
    for (int i = 0; i < 2; ++i) seg_w[i] = ui::TextSizeIn(sf, segs[i]).x + ui::Dp(14.0f);
    dl->AddRect(ImVec2(sx, sy), ImVec2(sx + seg_w[0] + seg_w[1], sy + seg_h), ui::kStroke, seg_h * 0.5f);
    for (int i = 0; i < 2; ++i) {
        const bool on = (i == 1) == ch.own;
        ImGui::SetCursorScreenPos(ImVec2(sx, sy));
        if (ImGui::InvisibleButton(i == 0 ? "##reaper" : "##own", ImVec2(seg_w[i], seg_h)) && !on) {
            if (i == 0) SetViewNavLinked(row);
            else SetViewNavOwn(row);
        }
        const bool hot = ImGui::IsItemHovered();
        if (hot) ui::ItemTipText(i == 0 ? "Follow REAPER: your binding of this action in REAPER, read live"
                                        : "Your own gesture for RAV only (double-click the row to record it)");
        if (on)
            dl->AddRectFilled(ImVec2(sx + 1.0f, sy + 1.0f), ImVec2(sx + seg_w[i] - 1.0f, sy + seg_h - 1.0f),
                              i == 0 ? ui::kAccentSoft : ui::kRaised, seg_h * 0.5f);
        const ImU32 ink = on ? (i == 0 ? ui::kAccentText : ui::kText) : (hot ? ui::kText : ui::kFaint);
        const ImVec2 ts = ui::TextSizeIn(sf, segs[i]);
        ui::AddTextIn(dl, sf, ImVec2(sx + std::floor((seg_w[i] - ts.x) * 0.5f), sy + std::floor((seg_h - ts.y) * 0.5f)), ink, segs[i]);
        sx += seg_w[i];
    }
    std::string src;
    if (!ch.own) {
        src = std::string("\xE2\x9B\x93 ") + def.reaper_name + " \xC2\xB7 " + std::to_string(def.reaper_cmd);
    } else {
        std::string re;
        for (const ViewGesture& g : ReaperViewGestures(CurrentReaperViewKeys(), row)) re += (re.empty() ? "" : ", ") + ViewGestureLabel(g);
        src = "RAV only \xC2\xB7 REAPER's would be " + (re.empty() ? std::string("none") : re);
    }
    {
        ImGui::PushFont(sf);
        const ImVec2 tp(sx + ui::Dp(6.0f), sy + std::floor((seg_h - ImGui::GetTextLineHeight()) * 0.5f));
        const bool   cut = ui::EllipsisText(dl, tp, b.x - ui::Dp(8.0f), ui::kFaint, src);
        ImGui::PopFont();
        if (cut && ImGui::IsMouseHoveringRect(ImVec2(tp.x, sy), ImVec2(b.x, sy + seg_h)) && ImGui::IsWindowHovered()) {
            ImGui::SetTooltip("%s", src.c_str());
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();
    return record;
}

// 13-45: the record modal of a "View: scroll and zoom" row: a key or the wheel; a gesture another row
// runs waits for Keep (it is then taken from that row).
void NavRecordBody(int row)
{
    const ViewNavDef& def = kViewNavTable[row];
    ImGui::Text(def.wheel_ok ? "Press a key or turn the wheel for %s..." : "Press a key for %s...", def.label);
    ui::Caption("Ctrl, Shift and Alt can be held with it. Esc to cancel.");
    const float wrap = ui::Dp(320.0f);
    ViewGesture g;
    int         owner = -1;
    const bool  pending = ViewNavPending(&g, &owner);
    std::string label;
    if (pending) {
        label = ViewGestureLabel(g);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ui::KeyCap(ImGui::GetWindowDrawList(), p, label.c_str(), ui::kAccent);
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight() + ui::Dp(3.0f)));
        const bool linked = owner >= 0 && !CurrentViewNavChoices()[static_cast<size_t>(owner)].own;
        ui::ToneText(std::string("Already used by ") + (owner >= 0 ? kViewNavTable[owner].label : "another action") +
                         (linked ? " (linked to REAPER)" : " (your own)") + ". Keeping it takes it from that one.",
                     ui::kWarn, /*wrap=*/true, wrap);
    } else if (ViewNavRefusal()[0]) {
        ui::ToneText(ViewNavRefusal(), ui::kWarn, /*wrap=*/true, wrap);
    } else {
        ImGui::TextUnformatted(" ");  // keeps the modal's size when a warning shows
    }
    ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
    if (ui::SolidButton("Cancel", ImVec2(ui::Dp(90.0f), 0.0f))) {
        CancelShortcutRecording();
        ImGui::CloseCurrentPopup();
    }
    if (pending) {
        ImGui::SameLine();
        if (ui::SolidButton(("Keep " + label + "##navkeep").c_str())) ViewNavKeepPending();  // ends the recording
    }
}

// The group's note: where linked rows read REAPER's bindings, and when.
std::string NavKbNote()
{
    const double age = ViewNavKbAge();
    if (age < 0.0) return "Linked rows follow REAPER: no reaper-kb.ini found, so REAPER's default bindings.";
    char when[48];
    if (age < 60.0) std::snprintf(when, sizeof(when), "%d s ago", static_cast<int>(age));
    else if (age < 3600.0) std::snprintf(when, sizeof(when), "%d min ago", static_cast<int>(age / 60.0));
    else std::snprintf(when, sizeof(when), "%d h ago", static_cast<int>(age / 3600.0));
    return std::string("Linked rows follow REAPER: read from reaper-kb.ini ") + when +
           " (it is read again when REAPER saves it).";
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
        } else if (id >= kShortcutCount) {
            NavRecordBody(id - kShortcutCount);  // 13-45: a view row
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
    if (record >= 0) {
        BeginShortcutRecording(record);
        g_reset_refused = -1;
    }
    // A refused restore stays explained while that action still has no key.
    if (g_reset_refused >= 0 && CurrentShortcuts()[static_cast<size_t>(g_reset_refused)].vk == 0 && g_reset_blocker >= 0) {
        const std::string def = FormatShortcut(kShortcutTable[g_reset_refused].def);
        ui::ToneText(std::string("\"") + kShortcutTable[g_reset_refused].label + "\" cannot get " + def +
                         " back: it is your key for \"" + kShortcutTable[g_reset_blocker].label +
                         "\". Give that action another key first, or double-click to pick another key.",
                     ui::kWarn, /*wrap=*/true, ui::Dp(kRowW));
    } else {
        g_reset_refused = -1;
    }

    // 13-45: the six view actions of every timeline (the mockup's option A).
    Header("VIEW: SCROLL AND ZOOM", "every timeline in RAV");
    Note("Sound Edit, Blend lanes, the Video and Tagging strips. REAPER: your binding of that REAPER action, "
         "live. Own: yours, for RAV only (double-click to record it; right-click: back to REAPER).");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Dp(8.0f), ui::Dp(2.0f)));
    int nav_record = -1;
    for (int r = 0; r < kNavRowCount; ++r)
        if (NavRow(r)) nav_record = r;
    ImGui::PopStyleVar();
    Note(NavKbNote().c_str());
    if (nav_record >= 0) {
        CancelShortcutRecording();
        BeginViewNavRecording(nav_record);
        g_reset_refused = -1;
    }

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

    RecordModal(record >= 0 || nav_record >= 0);
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
