// SPDX-License-Identifier: MIT
//
// See video_view_ui.h.

#include "video_view_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include "reaper_actions.h"  // spec 11-fb-5: REAPER's Video window open
#include "shortcuts.h"   // spec 11-fb-3: the current keys on the key caps
#include "ui_theme.h"
#include "video_preview_lag.h"
#include "video_shot_timing.h"

namespace rav {
namespace {

constexpr float kPanelWidth = 300.0f;
constexpr float kPanelGap   = 8.0f;

// Strings outside ASCII are spelled as UTF-8 escapes (the build has no /utf-8).
#define RAV_DOT   "\xC2\xB7"   // middle dot
#define RAV_TIMES "\xC3\x97"   // multiplication sign
#define RAV_DEG   "\xC2\xB0"   // degree sign

constexpr ImGuiWindowFlags kOverlayFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
    ImGuiWindowFlags_NoBackground;

void FormatTime(double t, char* out, size_t cap)
{
    if (!std::isfinite(t) || t < 0.0) t = 0.0;
    // Round to the millisecond first: 59.9996 s prints 1:00.000, never 0:60.000.
    const long long ms = std::llround(t * 1000.0);
    const int minutes = static_cast<int>(ms / 60000);
    const double seconds = static_cast<double>(ms % 60000) / 1000.0;
    std::snprintf(out, cap, "%d:%06.3f", minutes, seconds);
}

void FormatFps(double fps, char* out, size_t cap)
{
    if (fps <= 0.0) {
        std::snprintf(out, cap, "project fps");
        return;
    }
    if (std::fabs(fps - std::round(fps)) < 0.005) std::snprintf(out, cap, "%.0f fps", fps);
    else {
        // 29.97 / 23.976 / 59.94: three decimals, trailing zeros dropped ("%.3g" said "30").
        char num[32];
        std::snprintf(num, sizeof(num), "%.3f", fps);
        size_t n = std::strlen(num);
        while (n > 0 && num[n - 1] == '0') num[--n] = '\0';
        if (n > 0 && num[n - 1] == '.') num[--n] = '\0';
        std::snprintf(out, cap, "%s fps", num);
    }
}

// What the frame and the panel say when Video view cannot show the camera.
struct StateText {
    const char* title = "";
    std::string body;
};

StateText StateMessage(const VideoViewModel& m)
{
    StateText s;
    switch (m.status) {
        case VideoFxStatus::NoTrack:
            s.title = "No animation to frame";
            s.body = "Move the playhead over an animation item: Video view follows its track.";
            break;
        case VideoFxStatus::Missing:
            s.title = "No video FX on this track";
            s.body = m.track_label + ". Add it to frame and render this animation; RAV manages it for you.";
            break;
        case VideoFxStatus::Bypassed:
            s.title = "Video FX bypassed";
            s.body = "The FX on " + m.track_label + " is bypassed, so the video shows nothing from it.";
            break;
        case VideoFxStatus::Offline:
            s.title = "Video FX offline";
            s.body = "The FX on " + m.track_label + " is offline, so the video shows nothing from it.";
            break;
        case VideoFxStatus::OutOfDate:
            s.title = "Video FX inactive";
            s.body = "The video FX and the RAV extension come from different releases, so the FX shows "
                     "nothing and lower video tracks show through. Update ReaAnimViewer (ReaPack users "
                     "update the package; Demute Reaper Toolkit users click Run on the ReaAnimViewer "
                     "card), then restart REAPER.";
            break;
        case VideoFxStatus::Active:
            break;
    }
    return s;
}

// The button that fixes the state, if any. Queues the fix (it runs outside the frame).
void FixButton(VideoFxStatus status, float width)
{
    switch (status) {
        case VideoFxStatus::Missing:
            if (ui::PrimaryButton("Add video FX to track", ImVec2(width, 0.0f))) QueueVideoAddFx();
            break;
        case VideoFxStatus::Bypassed:
            if (ui::SolidButton("Enable FX", ImVec2(width, 0.0f))) QueueVideoEnableFx();
            break;
        case VideoFxStatus::Offline:
            if (ui::SolidButton("Set FX online", ImVec2(width, 0.0f))) QueueVideoSetFxOnline();
            break;
        default:
            break;
    }
}

void PanelIcon(ImDrawList* dl, ImVec2 c, ImU32 ink)
{
    // The mock-up's panel glyph: a window with a right column and three lines in it.
    const ImVec2 a(c.x - 6.5f, c.y - 5.5f);
    const ImVec2 b(c.x + 6.5f, c.y + 5.5f);
    dl->AddRect(a, b, ink, 2.0f, 0, 1.4f);
    dl->AddLine(ImVec2(c.x + 2.0f, a.y), ImVec2(c.x + 2.0f, b.y), ink, 1.4f);
    for (int i = 0; i < 3; ++i) {
        const float y = c.y - 2.5f + 2.0f * static_cast<float>(i);
        dl->AddLine(ImVec2(c.x + 3.8f, y), ImVec2(c.x + 4.8f, y), ink, 1.2f);
    }
}

void VideoIcon(ImDrawList* dl, ImVec2 p, ImU32 ink)
{
    // The mock-up's camera glyph: a body and a lens wedge.
    dl->AddRect(ImVec2(p.x + 1.5f, p.y + 3.5f), ImVec2(p.x + 11.0f, p.y + 12.5f), ink, 1.5f, 0, 1.4f);
    const ImVec2 w[4] = {ImVec2(p.x + 11.0f, p.y + 6.5f), ImVec2(p.x + 14.5f, p.y + 4.5f),
                         ImVec2(p.x + 14.5f, p.y + 11.5f), ImVec2(p.x + 11.0f, p.y + 9.5f)};
    dl->AddPolyline(w, 4, ink, 0, 1.4f);
}

// One inspector slider row, in display units. Returns nothing; edits go to the model.
void ParamRow(int p, const double values[vcam::kParamCount], bool enabled)
{
    static const char* const kLabels[vcam::kParamCount] = {"Yaw", "Pitch", "Distance", "Target X", "Target Y",
                                                           "Target Z"};
    float lo = -180.0f;
    float hi = 180.0f;
    const char* fmt = "%.0f" RAV_DEG;
    ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
    switch (p) {
        case vcam::kYaw: break;
        case vcam::kPitch:
            lo = -89.0f;
            hi = 89.0f;
            break;
        case vcam::kDistance:
            lo = static_cast<float>(vcam::kDistMin);
            hi = static_cast<float>(vcam::kDistMax);
            fmt = "%.2f" RAV_TIMES "r";
            flags |= ImGuiSliderFlags_Logarithmic;
            break;
        default:
            lo = static_cast<float>(-vcam::kTargetRange);
            hi = static_cast<float>(vcam::kTargetRange);
            fmt = "%+.2fr";
            break;
    }
    ImGui::PushID(p);
    ImGui::AlignTextToFramePadding();
    ui::Caption(kLabels[p]);
    ImGui::SameLine(70.0f);
    ImGui::SetNextItemWidth(-1.0f);
    float x = static_cast<float>(vcam::DisplayFromNorm(p, values[p]));
    if (ImGui::SliderFloat("##v", &x, lo, hi, fmt, flags) && enabled) VideoViewSliderEdit(p, vcam::NormFromDisplay(p, x));
    // Also when the row went disabled mid-drag: the slider gesture must end (one undo point).
    if (ImGui::IsItemDeactivatedAfterEdit()) VideoViewSliderCommit(p);
    ImGui::PopID();
}

// ---- Story 11-5 helpers ---------------------------------------------------------------------

// A dashed rectangle (the mock-up's .add and dashed chips).
void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col)
{
    constexpr float kDash = 4.0f;
    constexpr float kGap = 3.0f;
    for (float x = a.x; x < b.x; x += kDash + kGap) {
        const float x2 = std::min(x + kDash, b.x);
        dl->AddLine(ImVec2(x, a.y + 0.5f), ImVec2(x2, a.y + 0.5f), col, 1.0f);
        dl->AddLine(ImVec2(x, b.y - 0.5f), ImVec2(x2, b.y - 0.5f), col, 1.0f);
    }
    for (float y = a.y; y < b.y; y += kDash + kGap) {
        const float y2 = std::min(y + kDash, b.y);
        dl->AddLine(ImVec2(a.x + 0.5f, y), ImVec2(a.x + 0.5f, y2), col, 1.0f);
        dl->AddLine(ImVec2(b.x - 0.5f, y), ImVec2(b.x - 0.5f, y2), col, 1.0f);
    }
}

// A full-width dashed button with a label and an optional key cap (the mock-up's .add).
bool DashedButton(const char* id, const char* label, const char* key, float width, float height, bool enabled)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool disabled = !enabled;
    const ImVec2 p1(p0.x + width, p0.y + height);
    if (hovered && !disabled) dl->AddRectFilled(p0, p1, ui::kRaised, ui::kRadiusMd);
    DashedRect(dl, p0, p1, ui::kStrokeStrong);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const float key_w = key ? ImGui::CalcTextSize(key).x + 8.0f + 8.0f : 0.0f;
    // A long custom key ("Ctrl+Shift+Alt+PageDown") must not push the label off the button.
    const float x = std::max(p0.x + 6.0f, p0.x + (width - ts.x - key_w) * 0.5f);
    const float y = p0.y + (height - ts.y) * 0.5f;
    dl->AddText(ImVec2(x, y), (hovered && !disabled) ? ui::kText : ui::kMuted, label);
    if (key) ui::KeyCap(dl, ImVec2(x + ts.x + 8.0f, y - 1.5f), key);
    return pressed;
}

// A group title: a caption and a faint note on the same line (the mock-up's .grp).
void GroupHeader(const char* title, const char* note)
{
    ImGui::Spacing();
    ui::Caption(title);
    if (note && note[0]) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kFaint));
        ImGui::TextUnformatted(note);
        ImGui::PopStyleColor();
    }
}

// The notice under an action that did nothing (e.g. a cut on a frame that already starts a shot).
void NoticeLine()
{
    if (const char* n = VideoViewNotice()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kWarn));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(n);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
}

// The shot list (UX decision 5 / Story 11-5): one card per shot -- colour, name, Cut/Move,
// start time, delete (not on the first shot). A click moves the playhead to the shot.
void ShotList(const VideoViewModel& m, float full)
{
    char count[32];
    std::snprintf(count, sizeof(count), "%d shot%s", static_cast<int>(m.shots.size()), m.shots.size() == 1 ? "" : "s");
    GroupHeader("Shots", count);

    const int current = VideoViewShotIndex();
    const float row_h = 28.0f;
    ImGui::PushStyleColor(ImGuiCol_Header, ui::Col(ui::kAccentSoft));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui::Col(ui::kRaised));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ui::Col(ui::kAccentSoft));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));
    for (int i = 0; i < static_cast<int>(m.shots.size()); ++i) {
        const VideoShot& s = m.shots[static_cast<size_t>(i)];
        ImGui::PushID(i);
        const bool sel = (i == current);
        if (ImGui::Selectable("##shotrow", sel, ImGuiSelectableFlags_AllowOverlap, ImVec2(full, row_h)))
            QueueVideoSeekToShot(i);
        if (ImGui::IsItemHovered() && !ImGui::IsAnyItemActive()) ImGui::SetTooltip("Move the playhead to this shot");
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (sel) dl->AddRect(a, b, ui::kAccentLine, ui::kRadiusMd);
        const float cy = (a.y + b.y) * 0.5f;
        const float th = ImGui::GetTextLineHeight();
        dl->AddCircleFilled(ImVec2(a.x + 11.0f, cy), 4.0f, ui::kCurvePalette[i % 8], 12);

        // Right side first: delete, time, Cut/Move tag; the name takes what is left.
        const bool can_delete = (i > 0) && !s.implicit;
        float right = b.x - 4.0f;
        if (can_delete) {
            ImGui::SetCursorScreenPos(ImVec2(right - 22.0f, cy - 11.0f));
            if (ImGui::Button("x##del", ImVec2(22.0f, 22.0f))) QueueVideoDeleteShot(i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete this shot");
        }
        right -= 26.0f;  // same column with or without the button
        char tm[32];
        FormatTime(VideoShotFirstFrameTime(s.time, m.fps), tm, sizeof(tm));  // a cut sits half a frame early
        const float tm_w = ImGui::CalcTextSize(tm).x;
        dl->AddText(ImVec2(right - tm_w, cy - th * 0.5f), ui::kMuted, tm);
        right -= tm_w + 8.0f;
        const char* tag = s.move_to_next ? "Move" : "Cut";
        const float tag_w = ImGui::CalcTextSize(tag).x + 14.0f;
        const ImVec2 ta(right - tag_w, cy - th * 0.5f - 2.0f);
        const ImVec2 tb(right, cy + th * 0.5f + 2.0f);
        dl->AddRectFilled(ta, tb, ui::kBg, (tb.y - ta.y) * 0.5f);
        dl->AddRect(ta, tb, ui::kStroke, (tb.y - ta.y) * 0.5f);
        dl->AddText(ImVec2(ta.x + 7.0f, cy - th * 0.5f), ui::kMuted, tag);
        right = ta.x - 6.0f;
        const float name_x = a.x + 22.0f;
        if (right > name_x) {
            dl->PushClipRect(ImVec2(name_x, a.y), ImVec2(right, b.y), true);
            dl->AddText(ImVec2(name_x, cy - th * 0.5f), ui::kText, s.name.c_str());
            dl->PopClipRect();
        }
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 2.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    const bool can_cut = VideoViewCanCut();
    if (!can_cut) ImGui::BeginDisabled();
    const char* cut_key = ShortcutKeyLabel(kShortcutCut);
    if (DashedButton("##cutpanel", "+ Cut at playhead", cut_key, full, 30.0f, can_cut)) QueueVideoCut();
    if (!can_cut) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A new shot starts on the playhead's frame, with the camera shown there (%s in Video view)",
                          cut_key);
    NoticeLine();
}

// Saved angles (Story 11-5): chips that put an angle into the shot under the playhead,
// right-click to delete, + Save to keep the current shot's camera under a name.
void SavedAngles(const VideoViewModel& m, float full)
{
    GroupHeader("Saved angles", nullptr);
    const bool can_apply = (m.status == VideoFxStatus::Active) && !m.shots.empty();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float chip_h = 24.0f;
    const float start_x = ImGui::GetCursorPosX();
    float line_w = 0.0f;
    static std::string s_ctx_name;
    bool open_ctx = false;

    auto place = [&](float w) {
        if (line_w > 0.0f && line_w + 4.0f + w <= full) {
            ImGui::SameLine(0.0f, 4.0f);
            line_w += 4.0f + w;
        } else {
            if (line_w > 0.0f) ImGui::SetCursorPosX(start_x);
            line_w = w;
        }
    };

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, chip_h * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStroke));
    for (int i = 0; i < static_cast<int>(m.angles.size()); ++i) {
        const VideoFxAngle& a = m.angles[static_cast<size_t>(i)];
        const float w = std::min(full, ImGui::CalcTextSize(a.name.c_str()).x + 2.0f * st.FramePadding.x + 4.0f);
        place(w);
        ImGui::PushID(i);
        if (!can_apply) ImGui::BeginDisabled();
        if (ImGui::Button(a.name.c_str(), ImVec2(w, chip_h))) QueueVideoApplyAngle(i);
        if (!can_apply) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%.0f" RAV_DEG " yaw, %.0f" RAV_DEG " pitch, %.2f" RAV_TIMES "r\n"
                              "Click: apply to the shot under the playhead. Right-click: delete.",
                              vcam::DisplayFromNorm(vcam::kYaw, a.values[vcam::kYaw]),
                              vcam::DisplayFromNorm(vcam::kPitch, a.values[vcam::kPitch]),
                              vcam::DisplayFromNorm(vcam::kDistance, a.values[vcam::kDistance]));
        }
        // Right-click works on a disabled chip too (IsItemClicked ignores disabled items).
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            s_ctx_name = a.name;
            open_ctx = true;  // opened outside PushID: BeginPopup below uses the window's ID stack
        }
        ImGui::PopID();
    }
    if (open_ctx) ImGui::OpenPopup("##anglectx");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // + Save (dashed chip): asks a name, then saves the shot under the playhead.
    static char s_new_name[kVideoFxMaxName + 1] = {};
    {
        const char* label = "+ Save";
        const float w = ImGui::CalcTextSize(label).x + 2.0f * st.FramePadding.x + 4.0f;
        place(w);
        const bool can_save = !m.shots.empty() && m.fx >= 0;
        if (!can_save) ImGui::BeginDisabled();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##saveangle", ImVec2(w, chip_h))) {
            // A free default name: "Angle N".
            for (int n = static_cast<int>(m.angles.size()) + 1;; ++n) {
                std::snprintf(s_new_name, sizeof(s_new_name), "Angle %d", n);
                bool taken = false;
                for (const VideoFxAngle& a : m.angles) taken = taken || a.name == s_new_name;
                if (!taken) break;
            }
            ImGui::OpenPopup("##saveanglepop");
        }
        const bool hovered = ImGui::IsItemHovered();
        if (!can_save) ImGui::EndDisabled();
        const ImVec2 p1(p0.x + w, p0.y + chip_h);
        if (hovered && can_save) dl->AddRectFilled(p0, p1, ui::kRaised, chip_h * 0.5f);
        DashedRect(dl, ImVec2(p0.x + 3.0f, p0.y), ImVec2(p1.x - 3.0f, p1.y), ui::kStrokeStrong);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y + (chip_h - ts.y) * 0.5f),
                    (hovered && can_save) ? ui::kText : ui::kMuted, label);
        if (hovered) ImGui::SetTooltip("Save the camera of the shot under the playhead as an angle");
    }

    if (ImGui::BeginPopup("##anglectx")) {
        const std::string label = "Delete \"" + s_ctx_name + "\"";
        if (ImGui::Selectable(label.c_str())) QueueVideoDeleteAngle(s_ctx_name);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##saveanglepop")) {
        ImGui::TextUnformatted("Save this shot's camera as");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(220.0f);
        const bool enter = ImGui::InputText("##anglename", s_new_name, sizeof(s_new_name),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (ui::PrimaryButton("Save##angle", ImVec2(106.0f, 0.0f)) || enter) {
            QueueVideoSaveAngle(s_new_name);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (ui::SolidButton("Cancel##angle", ImVec2(106.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

struct SizePreset {
    int w;
    int h;
    const char* note;
};
constexpr SizePreset kSizePresets[] = {
    {1280, 720, ""},  {1920, 1080, ""}, {2560, 1440, ""},
    {3840, 2160, ""}, {1080, 1920, " " RAV_DOT " vertical"}, {1080, 1080, " " RAV_DOT " square"},
};

std::string SizeLabel(int w, int h, const char* note)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d " RAV_TIMES " %d%s", w, h, note ? note : "");
    return buf;
}

// The Output section (UX decision 6): size (project, or an override), frame rate (the
// project's), background (the viewer's, or transparent).
void OutputSection(const VideoViewModel& m)
{
    ImGui::Spacing();
    ImGui::Separator();
    GroupHeader("Output", nullptr);
    constexpr float kLabelX = 84.0f;

    // Size.
    std::string project_label;
    if (m.project_w > 0 && m.project_h > 0) project_label = "Project " RAV_DOT " " + SizeLabel(m.project_w, m.project_h, "");
    else project_label = "Project " RAV_DOT " not set (1920 " RAV_TIMES " 1080)";
    const bool has_override = m.override_w > 0 && m.override_h > 0;
    int preset_index = -1;
    for (int i = 0; has_override && i < static_cast<int>(sizeof(kSizePresets) / sizeof(kSizePresets[0])); ++i) {
        if (kSizePresets[i].w == m.override_w && kSizePresets[i].h == m.override_h) preset_index = i;
    }
    std::string preview = project_label;
    if (has_override) {
        preview = preset_index >= 0 ? SizeLabel(kSizePresets[preset_index].w, kSizePresets[preset_index].h,
                                                kSizePresets[preset_index].note)
                                    : SizeLabel(m.override_w, m.override_h, " " RAV_DOT " custom");
    }
    ImGui::AlignTextToFramePadding();
    ui::Caption("Size");
    ImGui::SameLine(kLabelX);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##outsize", preview.c_str())) {
        if (ImGui::Selectable(project_label.c_str(), !has_override) && has_override) QueueVideoOutputSize(0, 0);
        for (int i = 0; i < static_cast<int>(sizeof(kSizePresets) / sizeof(kSizePresets[0])); ++i) {
            const SizePreset& p = kSizePresets[i];
            ImGui::PushID(i);
            if (ImGui::Selectable(SizeLabel(p.w, p.h, p.note).c_str(), i == preset_index) && i != preset_index)
                QueueVideoOutputSize(p.w, p.h);
            ImGui::PopID();
        }
        if (has_override && preset_index < 0)
            ImGui::Selectable(SizeLabel(m.override_w, m.override_h, " " RAV_DOT " custom").c_str(), true);
        if (ImGui::Selectable("Custom...")) QueueVideoCustomOutputSize();
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The size of the rendered picture. Project = Project Settings > Video.");

    // Frame rate (read only: REAPER renders at the project's rate).
    ImGui::AlignTextToFramePadding();
    ui::Caption("Frame rate");
    ImGui::SameLine(kLabelX);
    char fps[32];
    if (m.fps > 0.0) FormatFps(m.fps, fps, sizeof(fps));
    else std::snprintf(fps, sizeof(fps), "not set");
    ImGui::TextUnformatted(fps);
    ImGui::SameLine(0.0f, 4.0f);
    ui::Caption(RAV_DOT " project");

    // Background.
    ImGui::AlignTextToFramePadding();
    ui::Caption("Background");
    ImGui::SameLine(kLabelX);
    ImGui::SetNextItemWidth(-1.0f);
    static const char* const kBackgrounds[2] = {"Viewer background", "Transparent"};
    const int bg = m.transparent ? 1 : 0;
    if (ImGui::BeginCombo("##outbg", kBackgrounds[bg])) {
        for (int i = 0; i < 2; ++i) {
            if (ImGui::Selectable(kBackgrounds[i], i == bg) && i != bg) QueueVideoBackground(i == 1);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Transparent: where there is no model, the video shows the tracks below (if REAPER "
                          "composites the FX's transparency). Light, floor, grid, shadow and MSAA follow the "
                          "View menu.");
}

// The render row (UX decision 6): REAPER's own windows render picture and sound. One line,
// two equal buttons, pinned at the top of the panel (outside its scrolling body).
void RenderRow(float full)
{
    constexpr float kGap = 6.0f;  // between the two buttons
    const float half = std::floor((full - kGap) * 0.5f);
    if (ui::SolidButton("Matrix##render", ImVec2(half, 0.0f))) QueueVideoOpenRenderMatrix();  // same style as Render (Antho)
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("REAPER's Region Render Matrix: tick regions, then render from the Render dialog "
                          "(Source: Region render matrix, a video format)");
    ImGui::SameLine(0.0f, kGap);
    if (ui::SolidButton("Render##render", ImVec2(full - half - kGap, 0.0f))) QueueVideoOpenRenderDialog();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("REAPER's Render dialog: pick a video format (e.g. MP4) and the bounds");
}

}  // namespace

float VideoTopBand()
{
    // overlay row, a small gap, the caption line, the 5 px gap the captions keep above the frame
    // (called before ImGui's frame starts: without a context or a font size yet, assume 13 px)
    const float line = (ImGui::GetCurrentContext() && ImGui::GetFontSize() > 0.0f) ? ImGui::GetTextLineHeight() : 13.0f;
    return kVideoOverlayRowBottom + 4.0f + line + kVideoCaptionGap;
}

float VideoPanelFootprint(int client_w)
{
    if (!VideoPanelVisible()) return 0.0f;
    const float w = std::min(kPanelWidth, std::max(160.0f, static_cast<float>(client_w) * 0.6f));
    return w + 2.0f * kPanelGap;
}

void DrawVideoViewToggle(float center_x)
{
    ImGui::SetNextWindowPos(ImVec2(center_x, 8.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("##viewtoggle", nullptr, kOverlayFlags)) {
        static const char* const kViews[2] = {"RAV view", "Video view"};
        int sel = VideoViewActive() ? 1 : 0;
        if (ui::Segmented("##view", kViews, 2, &sel)) SetVideoViewActive(sel == 1);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawVideoPanelButton(float right_x)
{
    ImGui::SetNextWindowPos(ImVec2(right_x, 8.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("##panelbtn", nullptr, kOverlayFlags)) {
        const bool on = VideoPanelVisible();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 size(28.0f, 26.0f);
        if (ImGui::InvisibleButton("##panel", size)) SetVideoPanelVisible(!on);
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);
        dl->AddRectFilled(p0, p1, on ? ui::kAccentSoft : (hovered ? ui::kHover : IM_COL32(18, 19, 23, 184)), ui::kRadiusMd);
        dl->AddRect(p0, p1, on ? ui::kAccentLine : ui::kStroke, ui::kRadiusMd);
        PanelIcon(dl, ImVec2(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f), IM_COL32(0xC9, 0xCE, 0xD8, 0xFF));
        if (hovered) ImGui::SetTooltip("Show / hide the video settings (%s)", ShortcutKeyLabel(kShortcutTogglePanel));
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawVideoFrameDecor(const VideoFrameRect& fr, void (*copy_log)(), bool show_preview_lag)
{
    const VideoViewModel& m = GetVideoViewModel();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const ImVec2 a(fr.x, fr.y);
    const ImVec2 b(fr.x + fr.w, fr.y + fr.h);
    bg->AddRect(ImVec2(a.x - 1.0f, a.y - 1.0f), ImVec2(b.x + 1.0f, b.y + 1.0f), ui::kFrameStroke, 0.0f, 0, 1.0f);

    // Above the frame: size and frame rate on the left, the shot on the right.
    char fps[32];
    FormatFps(m.fps, fps, sizeof(fps));
    char left[96];
    std::snprintf(left, sizeof(left), "%d " RAV_TIMES " %d " RAV_DOT " %s", m.out_w, m.out_h, fps);
    const float text_h = ImGui::GetTextLineHeight();
    const ImVec2 left_pos(a.x, a.y - text_h - kVideoCaptionGap);
    bg->AddText(left_pos, ui::kMuted, left);

    // The shot on the right, measured first: the lag readout must not run into it.
    const int idx = VideoViewShotIndex();
    std::string right;
    if (m.status == VideoFxStatus::Active && idx >= 0 && idx < static_cast<int>(m.shots.size())) {
        const VideoShot& s = m.shots[static_cast<size_t>(idx)];
        right = s.name;
        if (s.move_to_next && idx + 1 < static_cast<int>(m.shots.size())) right += "  -> move";
    }
    float right_w = right.empty() ? 0.0f : ImGui::CalcTextSize(right.c_str()).x;

    // After the size, how long REAPER's Video window takes to catch up with an output size or
    // display change while playing (spec 11-fb-5). Shown while that window is open, Video
    // view is on and the FX is Active. Never hidden for lack of room: the full form if it fits
    // before the shot caption, else the compact one; when even that does not fit, the shot
    // caption is shortened ("...") or, with no room left at all, dropped. Clipped to the frame
    // width as a last resort. Hover = what it means, and that renders have no offset.
    if (show_preview_lag && m.status == VideoFxStatus::Active) {
        double lag = 0.0;
        bool live = false;
        PreviewLagTransport t;
        t.play_pos = VideoViewPlayhead();
        t.playing = VideoViewPlaying();
        VideoViewLoop(&t.looping, &t.loop_start, &t.loop_end);
        t.playrate = VideoViewPlayRate();
        bool window_known = false;
        const bool window_open = ReaperVideoWindowOpen(&window_known);
        if (VideoPreviewLag(m.track, t, window_known, window_open, &lag, &live)) {
            char value[32];
            if (live) std::snprintf(value, sizeof(value), ": live");
            else std::snprintf(value, sizeof(value), " %.1f s", lag);
            char full[64];
            char compact[64];
            std::snprintf(full, sizeof(full), " " RAV_DOT " REAPER catch-up%s", value);
            std::snprintf(compact, sizeof(compact), " " RAV_DOT " catch-up%s", value);
            const ImVec2 pos(left_pos.x + ImGui::CalcTextSize(left).x, left_pos.y);
            const float gap = ImGui::CalcTextSize("  ").x;
            const float limit = right.empty() ? b.x : b.x - right_w - gap;
            const char* text = full;
            ImVec2 size = ImGui::CalcTextSize(full);
            if (pos.x + size.x > limit) {
                text = compact;
                size = ImGui::CalcTextSize(compact);
                if (!right.empty() && pos.x + size.x > limit) {
                    // Shorten the shot caption so the compact readout fits; no room at all:
                    // drop it.
                    const float room = b.x - (pos.x + size.x) - gap;
                    const char* dots = "...";
                    const float dots_w = ImGui::CalcTextSize(dots).x;
                    std::string cut;
                    if (room > dots_w) {
                        size_t n = right.size();
                        while (n > 0) {
                            --n;
                            while (n > 0 && (static_cast<unsigned char>(right[n]) & 0xC0) == 0x80) --n;  // UTF-8
                            const std::string head = right.substr(0, n);
                            if (ImGui::CalcTextSize(head.c_str()).x + dots_w <= room) {
                                cut = head + dots;
                                break;
                            }
                        }
                        if (cut.empty()) cut = dots;
                    }
                    right = cut;
                    right_w = right.empty() ? 0.0f : ImGui::CalcTextSize(right.c_str()).x;
                }
            }
            // Last resort (frame narrower than size + readout): clipped to the frame width.
            const ImVec4 clip(a.x, pos.y, b.x, pos.y + size.y);
            bg->AddText(nullptr, 0.0f, pos, ui::kMuted, text, nullptr, 0.0f, &clip);
            if (ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), false) &&
                !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
                ImGui::TextUnformatted(
                    "After an output size or display change (light, floor, background...), "
                    "REAPER's Video window catches up after about this delay while playing. Camera "
                    "moves and playback stay in sync. Rendering has no offset: every frame is drawn for "
                    "its own time. Stopped, 'live': REAPER's window follows at once.");
                if (!window_known)
                    ImGui::TextUnformatted("(Video window state unknown: shown while REAPER asks for frames)");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    }

    if (!right.empty()) bg->AddText(ImVec2(b.x - right_w, a.y - text_h - kVideoCaptionGap), ui::kMuted, right.c_str());

    if (VideoViewCanEdit()) return;  // the picture: nothing is drawn inside the frame

    // A state the FX cannot render (UX decision 7): say why and offer the fix, centred in
    // the frame. Active FX without an item / model: a faint line only.
    if (m.status == VideoFxStatus::Active) {
        const char* line = "No item on this track at the playhead: the video shows nothing here.";
        const ImVec2 ts = ImGui::CalcTextSize(line);
        if (ts.x < fr.w - 16.0f) bg->AddText(ImVec2(a.x + (fr.w - ts.x) * 0.5f, a.y + (fr.h - ts.y) * 0.5f), ui::kFaint, line);
        else bg->AddText(nullptr, 0.0f, ImVec2(a.x + 8.0f, a.y + fr.h * 0.5f), ui::kFaint, line, nullptr, fr.w - 16.0f);
        return;
    }
    const StateText st = StateMessage(m);
    const float box_w = std::min(300.0f, std::max(160.0f, fr.w - 24.0f));
    ImGui::SetNextWindowPos(ImVec2(a.x + fr.w * 0.5f, a.y + fr.h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(box_w, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Col(ui::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStrokeStrong));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    constexpr ImGuiWindowFlags kBoxFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                           ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##framemsg", nullptr, kBoxFlags)) {
        ImGui::TextUnformatted(st.title);
        ui::SubText(st.body.c_str());
        NoticeLine();  // e.g. C pressed with no active FX: the strip that shows notices is hidden
        FixButton(m.status, -1.0f);
        if (m.status == VideoFxStatus::OutOfDate && copy_log && ui::SolidButton("Copy error log##frame", ImVec2(-1.0f, 0.0f)))
            copy_log();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

bool VideoShotStripVisible()
{
    return VideoViewActive() && GetVideoViewModel().status == VideoFxStatus::Active;
}

void DrawVideoShotStrip(float x, float y, float w, float h)
{
    const VideoViewModel& m = GetVideoViewModel();
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(std::max(w, 120.0f), h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Col(ui::kSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStroke));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##shotstrip", nullptr, kFlags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const double playhead = VideoViewPlayhead();
        const float lane_h = 24.0f;
        // Spec 11-fb-6 / 11-fb-9: a seconds / frames ruler over the lane, its labels at the full
        // font size, over a row of tick room at the bottom of the band for the minor ticks.
        ImFont* const ruler_font = ImGui::GetFont();
        constexpr float kRulerTickRoom = 6.0f;      // band height under the labels (minor ticks)
        constexpr float kRulerMinorTickH = 4.0f;    // minor tick height: inside the tick room
        constexpr float kRulerMinorMinPx = 4.0f;    // minor ticks at least this far apart
        constexpr float kRulerLabelPad = 3.0f;      // label text starts this far right of its tick
        constexpr float kRulerLabelGap = 12.0f;     // free space after the widest label
        static_assert(kRulerMinorTickH <= kRulerTickRoom, "minor ticks stay under the labels");
        // The band is clamped so lane + ruler always fit the strip (a large font shrinks it).
        const float ruler_h = std::max(0.0f, std::min(std::floor(ImGui::GetFontSize()) + kRulerTickRoom,
                                                      ImGui::GetContentRegionAvail().y - lane_h));
        const float ruler_fs = ruler_h - kRulerTickRoom;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float top = ImGui::GetCursorScreenPos().y +
                          std::max(0.0f, (ImGui::GetContentRegionAvail().y - lane_h - ruler_h) * 0.5f) + ruler_h;

        // Timecode of the playhead.
        char tc[32];
        FormatTime(playhead, tc, sizeof(tc));
        const float th = ImGui::GetTextLineHeight();
        const ImVec2 tc_pos(ImGui::GetCursorScreenPos().x, top + (lane_h - th) * 0.5f);
        dl->AddText(tc_pos, ui::kMuted, tc);
        const float tc_w = std::max(64.0f, ImGui::CalcTextSize(tc).x) + 8.0f;

        // Snap toggle (spec 11-fb-6), left of Cut: off by default, for the session only.
        static bool s_snap = false;
        const float snap_w = ImGui::CalcTextSize("Snap").x + 18.0f;

        // Cut button on the right (label + key cap).
        const char* cut_key = ShortcutKeyLabel(kShortcutCut);
        // A long custom key must not widen the button past the strip: it gets the room left.
        const float cut_w = std::max(20.0f, std::min(ImGui::CalcTextSize("Cut").x + ImGui::CalcTextSize(cut_key).x +
                                                         8.0f + 26.0f,
                                                     avail - tc_w - 20.0f - 8.0f - snap_w - 6.0f));
        const float lane_w = std::max(20.0f, avail - tc_w - cut_w - 8.0f - snap_w - 6.0f);
        const ImVec2 l0(tc_pos.x + tc_w, top);
        const ImVec2 l1(l0.x + lane_w, top + lane_h);

        // The lane: shots over the item, the playhead; click or drag = move the playhead. A
        // press ON a junction between two shots (feedback 11-fb-2) drags it instead: the later
        // shot's start follows the mouse frame by frame, written on release (one undo point).
        ImGui::SetCursorScreenPos(l0);
        ImGui::InvisibleButton("##lane", ImVec2(lane_w, lane_h));
        const bool lane_active = ImGui::IsItemActive();
        const bool lane_hovered = ImGui::IsItemHovered();
        const bool lane_pressed = ImGui::IsItemActivated();
        const bool lane_released = ImGui::IsItemDeactivated();
        constexpr float kJunctionGrabPx = 4.0f;
        static int    s_junction = -1;         // shot whose start is dragged, -1 = none (a scrub)
        static double s_junction_orig = 0.0;   // its time when grabbed
        static double s_junction_time = 0.0;   // the snapped time shown while dragging
        static float  s_junction_press_x = 0.0f;
        static double s_junction_press_t = 0.0;   // mouse time at the press (the drag is a delta)
        static bool   s_junction_moved = false;
        static bool   s_junction_dropped = false; // grabbed, then lost to a model change: inert until release
        static bool   s_dblclick_hold = false;    // a double-click seeked to a shot: no scrub until release
        dl->AddRectFilled(l0, l1, ui::kBg, ui::kRadiusSm);
        double ruler_step = 0.0;  // 0: no item, no ruler (Snap is then disabled)
        const VideoStripRange r = VideoViewStripRange();
        if (!r.valid || !(r.end > r.start)) {
            const char* line = "No animation item on this track";
            const ImVec2 ts = ImGui::CalcTextSize(line);
            dl->AddText(ImVec2(l0.x + std::max(6.0f, (lane_w - ts.x) * 0.5f), top + (lane_h - ts.y) * 0.5f), ui::kFaint,
                        line);
        } else {
            const double span = r.end - r.start;
            auto x_of = [&](double t) { return l0.x + static_cast<float>((t - r.start) / span) * lane_w; };
            const int n = static_cast<int>(m.shots.size());
            const float mx = ImGui::GetIO().MousePos.x;
            const double f = std::min(1.0, std::max(0.0, static_cast<double>((mx - l0.x) / lane_w)));
            const double t_mouse = r.start + f * span;
            // The ruler's step: labelled ticks at least the widest label for this span apart (the
            // longest seconds label, or a "+Nf" frame label), so full-size labels never collide.
            // Ticks count whole frames from the item's first frame (VideoRulerTickTime); fps
            // unknown: seconds from r.start.
            float label_w;
            {
                const long long ifps = VideoRulerFps(m.fps);
                char frame_label[32];
                std::snprintf(frame_label, sizeof(frame_label), "+%lldf", ifps > 1 ? ifps - 1 : 0LL);
                const char* sec_label = span >= 36000.0 ? "00:00:00" : span >= 3600.0 ? "0:00:00"
                                      : span >= 600.0  ? "00:00"    : "0:00";
                label_w = std::max(ImGui::CalcTextSize(sec_label).x,
                                   ifps > 1 ? ImGui::CalcTextSize(frame_label).x : 0.0f);
            }
            ruler_step = VideoRulerStep(span, static_cast<double>(lane_w), m.fps,
                                        static_cast<double>(kRulerLabelPad + label_w + kRulerLabelGap));
            // Spec 11-fb-9: minor ticks under it, down to one per frame.
            const double ruler_minor =
                VideoRulerMinorStep(ruler_step, span, static_cast<double>(lane_w), m.fps, kRulerMinorMinPx);
            // Snap lands on the finest graduation drawn.
            const double snap_step = VideoRulerSnapStep(ruler_step, ruler_minor);
            // Just inside the item's end: at r.end a back-to-back next item would become the
            // strip's item, and a drag past the edge would walk item after item.
            const double t_max = r.end - std::min(1.0e-3, span * 0.5);
            const bool snap = s_snap && ruler_step > 0.0;

            // Spec 11-fb-10: a shot starts at its first frame. Its line is drawn (and grabbed)
            // there, not at its envelope points half a frame earlier. fps unknown: the shot time.
            auto first_of = [&](double shot_time) { return VideoShotFirstFrameTime(shot_time, m.fps); };
            // The junction within the grab distance of x: the start of shot i >= 1, drawn inside the lane.
            auto junction_at = [&](float px) {
                int best = -1;
                float best_d = kJunctionGrabPx;
                for (int i = 1; i < n; ++i) {
                    const VideoShot& s = m.shots[static_cast<size_t>(i)];
                    const double drawn = first_of(s.time);
                    if (s.implicit || !(drawn > r.start && drawn < r.end)) continue;
                    const float d = std::fabs(px - x_of(drawn));
                    if (d <= best_d) {
                        best_d = d;
                        best = i;
                    }
                }
                return best;
            };

            // The grabbed shot must still be the one at the grabbed time (the model refreshes).
            if (s_junction >= 0 &&
                (s_junction >= n ||
                 std::fabs(m.shots[static_cast<size_t>(s_junction)].time - s_junction_orig) > kVideoShotTimeTolerance)) {
                s_junction = -1;
                s_junction_dropped = true;  // never falls back to a scrub while the button is held
            }
            if (lane_pressed) {
                s_junction_dropped = false;
                // Only a writable junction can be grabbed (an active FX, no camera gesture running).
                s_junction = VideoViewCanCut() ? junction_at(mx) : -1;
                if (s_junction >= 0) {
                    s_junction_orig = m.shots[static_cast<size_t>(s_junction)].time;
                    s_junction_time = s_junction_orig;
                    s_junction_press_x = mx;
                    s_junction_press_t = t_mouse;
                    s_junction_moved = false;
                }
            }
            if (lane_active && s_junction >= 0) {
                // A click (moves under the drag threshold) is no move.
                if (std::fabs(mx - s_junction_press_x) >= ImGui::GetIO().MouseDragThreshold) s_junction_moved = true;
                if (s_junction_moved) {
                    const size_t j = static_cast<size_t>(s_junction);
                    const double next_limit = (j + 1 < m.shots.size()) ? m.shots[j + 1].time : r.end;
                    // The drag is a delta from the junction's own time, not the cursor's position
                    // (the grab may be 4 px off). Cuts sit half a frame early, so add half a frame
                    // to read the frame the junction is on, not the one before it.
                    const double half = m.fps > 0.0 ? 0.5 / m.fps : 0.0;
                    double t = s_junction_orig + (t_mouse - s_junction_press_t) + half;
                    // Snap: the nearest tick's frame becomes the shot's first frame.
                    if (snap) t = VideoRulerSnap(t, r.start, snap_step, m.fps);
                    const double snapped = VideoJunctionDragTime(t, m.shots[j - 1].time, next_limit, m.fps);
                    s_junction_time = std::isfinite(snapped) ? snapped : s_junction_orig;  // no room: stays
                }
            }
            if (lane_released && s_junction >= 0) {
                if (s_junction_moved) QueueVideoMoveShot(s_junction, s_junction_time);
                s_junction = -1;
            }
            const bool junction_drag = lane_active && s_junction >= 0;

            // Shot starts as drawn: at their first frames, the dragged one at its snapped time's.
            std::vector<double> times(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) times[static_cast<size_t>(i)] = first_of(m.shots[static_cast<size_t>(i)].time);
            // The highlight stays on the shot Video view shows (its camera does not change before
            // the release writes the move).
            const int current = VideoViewShotIndex();
            if (junction_drag) times[static_cast<size_t>(s_junction)] = first_of(s_junction_time);

            dl->PushClipRect(l0, l1, true);
            for (int i = 0; i < n; ++i) {
                const VideoShot& s = m.shots[static_cast<size_t>(i)];
                const double s_time = times[static_cast<size_t>(i)];
                const double s_end = (i + 1 < n) ? times[static_cast<size_t>(i) + 1] : r.end;
                const double a = std::max(i == 0 ? r.start : s_time, r.start);  // the first shot holds from the start
                const double b = std::min(s_end, r.end);
                if (!(b > a)) continue;
                const float xa = x_of(a);
                const float xb = x_of(b);
                if (i == current) dl->AddRectFilled(ImVec2(xa, l0.y), ImVec2(xb, l1.y), ui::kAccentSoft);
                if (s.move_to_next && i + 1 < n) {
                    const float xm = xa + (xb - xa) * 0.55f;
                    dl->AddRectFilledMultiColor(ImVec2(xm, l0.y), ImVec2(xb, l1.y), IM_COL32(255, 255, 255, 0),
                                                IM_COL32(255, 255, 255, 16), IM_COL32(255, 255, 255, 16),
                                                IM_COL32(255, 255, 255, 0));
                }
                if (b < r.end) dl->AddLine(ImVec2(xb, l0.y), ImVec2(xb, l1.y), ui::kStroke, 1.0f);
                // Label: dot + name, clipped to the segment.
                if (xb - xa > 14.0f) {
                    dl->PushClipRect(ImVec2(xa, l0.y), ImVec2(xb - 2.0f, l1.y), true);
                    const float cy = (l0.y + l1.y) * 0.5f;
                    dl->AddCircleFilled(ImVec2(xa + 9.0f, cy), 3.0f, ui::kCurvePalette[i % 8], 10);
                    dl->AddText(ImVec2(xa + 16.0f, cy - th * 0.5f), i == current ? ui::kText : ui::kMuted,
                                s.name.c_str());
                    dl->PopClipRect();
                }
            }
            if (junction_drag) {  // the dragged junction, over the segment edges
                const float jx = x_of(first_of(s_junction_time));
                dl->AddLine(ImVec2(jx, l0.y), ImVec2(jx, l1.y), ui::kAccent, 2.0f);
            }
            if (playhead >= r.start && playhead <= r.end) {
                const float px = x_of(playhead);
                dl->AddRectFilled(ImVec2(px - 1.0f, l0.y), ImVec2(px + 1.0f, l1.y), IM_COL32(0xDF, 0xE3, 0xEA, 0xFF));
            }
            dl->PopClipRect();

            // The ruler (spec 11-fb-6 / 11-fb-9): above the lane, in the item's time. Whole seconds
            // read "0:02", frame ticks between them "+12f", at the full font size. Never over the
            // shot names.
            if (ruler_step > 0.0) {
                const ImVec2 r0(l0.x, l0.y - ruler_h);
                dl->PushClipRect(ImVec2(r0.x - 1.0f, r0.y), ImVec2(l1.x + 1.0f, l0.y), true);
                // Minor ticks (spec 11-fb-9): short, in the tick room under the labels, and the
                // faintest stroke; those on a labelled tick are skipped (same frame grid).
                const long long per_label = VideoRulerMinorPerLabel(ruler_step, ruler_minor, m.fps);
                if (per_label > 1) {
                    const float minor_h = std::min(kRulerMinorTickH, ruler_h);
                    for (long long k = 0; k <= 100000; ++k) {
                        const double tick_t = VideoRulerTickTime(k, r.start, ruler_minor, m.fps);
                        if (tick_t > r.end + 1e-9) break;
                        if (k % per_label == 0) continue;  // a labelled tick
                        const float tick_x = x_of(tick_t);
                        dl->AddLine(ImVec2(tick_x + 0.5f, l0.y - minor_h), ImVec2(tick_x + 0.5f, l0.y),
                                    ui::kStrokeStrong, 1.0f);
                    }
                }
                for (long long k = 0; k <= 100000; ++k) {
                    const double tick_t = VideoRulerTickTime(k, r.start, ruler_step, m.fps);
                    if (tick_t > r.end + 1e-9) break;
                    const float tick_x = x_of(tick_t);
                    char label[32];
                    VideoRulerTickLabel(k, ruler_step, m.fps, label, sizeof(label));
                    const bool major = VideoRulerTickIsMajor(k, ruler_step, m.fps);
                    // Labelled ticks rise past the minor ones and read brighter (minor kStrokeStrong <
                    // frame kFaint < second kMuted on the dark surface): whole seconds the full band.
                    const float tick_h = major ? ruler_h : std::max(3.0f, std::floor(ruler_h * 0.6f));
                    dl->AddLine(ImVec2(tick_x + 0.5f, l0.y - tick_h), ImVec2(tick_x + 0.5f, l0.y),
                                major ? ui::kMuted : ui::kFaint, 1.0f);
                    if (ruler_fs >= 6.0f) {
                        dl->AddText(ruler_font, ruler_fs, ImVec2(tick_x + kRulerLabelPad, r0.y),
                                    major ? ui::kText : ui::kMuted,
                                    label);
                    }
                }
                dl->PopClipRect();
            }

            if (lane_active && s_junction < 0 && !s_junction_dropped && !s_dblclick_hold) {  // a scrub: passes over junctions
                static float s_last_mx = -1.0e9f;
                // Snap: the nearest tick's frame (the frame a snapped junction lands on).
                const double t_seek = snap ? VideoRulerSnap(t_mouse, r.start, snap_step, m.fps) : t_mouse;
                if (lane_pressed || mx != s_last_mx) QueueVideoSeek(std::min(t_seek, t_max));
                s_last_mx = mx;
            }
            // A double-click on a shot (not on a junction) puts the playhead on its first frame,
            // like a click in the panel's shot list. After the scrub: the last seek queued wins.
            // A shot that started before the item: its first frame inside the item.
            if (lane_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && junction_at(mx) < 0) {
                // The shot whose first frame is at or before the frame under the mouse (as drawn).
                const int shot = VideoCurrentShotIndex(m.shots, t_mouse, m.fps);
                if (shot >= 0) {
                    const double first = std::max(VideoShotFirstFrameTime(m.shots[static_cast<size_t>(shot)].time, m.fps),
                                                  VideoShotFirstFrameTime(r.start, m.fps));
                    QueueVideoSeek(std::min(first, t_max));
                    s_dblclick_hold = true;
                }
            }
            if (junction_drag) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            } else if (lane_hovered && !lane_active) {
                if (!ImGui::IsAnyMouseDown() && VideoViewCanCut() && junction_at(mx) >= 0) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);  // the cursor says it: no tooltip
                } else {
                    ImGui::SetTooltip("Click or drag to move the playhead");
                }
            }
        }
        if (!lane_active) {  // released, or the lane lost its item mid-drag: nothing written
            s_junction = -1;
            s_junction_dropped = false;
            s_dblclick_hold = false;
        }
        dl->AddRect(l0, l1, ui::kStroke, ui::kRadiusSm);

        // A notice (e.g. the cut was refused) over the lane.
        if (const char* notice = VideoViewNotice()) {
            const ImVec2 ts = ImGui::CalcTextSize(notice);
            const ImVec2 na(l0.x + std::max(4.0f, (lane_w - ts.x) * 0.5f - 8.0f), l0.y + 2.0f);
            const ImVec2 nb(std::min(l1.x - 2.0f, na.x + ts.x + 16.0f), l1.y - 2.0f);
            dl->AddRectFilled(na, nb, ui::kRaised, ui::kRadiusSm);
            dl->AddRect(na, nb, ui::kStrokeStrong, ui::kRadiusSm);
            dl->PushClipRect(na, nb, true);
            dl->AddText(ImVec2(na.x + 8.0f, top + (lane_h - ts.y) * 0.5f), ui::kWarn, notice);
            dl->PopClipRect();
        }

        // Snap (spec 11-fb-6): lit while on, like the panel button. No tooltip. Disabled (and
        // unlit) without an item or a ruler: there is nothing to snap to.
        {
            const bool snap_enabled = ruler_step > 0.0;
            const bool lit = s_snap && snap_enabled;
            const ImVec2 s0(l1.x + 8.0f, top);
            const ImVec2 s1(s0.x + snap_w, top + lane_h);
            ImGui::SetCursorScreenPos(s0);
            if (!snap_enabled) ImGui::BeginDisabled();
            if (ImGui::InvisibleButton("##stripsnap", ImVec2(snap_w, lane_h))) s_snap = !s_snap;
            const bool hovered = snap_enabled && ImGui::IsItemHovered();
            if (!snap_enabled) ImGui::EndDisabled();
            dl->AddRectFilled(s0, s1, lit ? ui::kAccentSoft : (hovered ? ui::kHover : ui::kRaised), ui::kRadiusSm);
            dl->AddRect(s0, s1, lit ? ui::kAccentLine : ui::kStroke, ui::kRadiusSm);
            const ImVec2 ts = ImGui::CalcTextSize("Snap");
            dl->AddText(ImVec2(s0.x + (snap_w - ts.x) * 0.5f, top + (lane_h - th) * 0.5f),
                        lit ? ui::kText : (snap_enabled ? ui::kMuted : ui::kFaint), "Snap");
        }

        // Cut (C by default).
        ImGui::SetCursorScreenPos(ImVec2(l1.x + 8.0f + snap_w + 6.0f, top));
        const bool can_cut = VideoViewCanCut();
        if (!can_cut) ImGui::BeginDisabled();
        const ImVec2 b0 = ImGui::GetCursorScreenPos();
        if (ui::SolidButton("##stripcut", ImVec2(cut_w, lane_h))) QueueVideoCut();
        if (!can_cut) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("New shot on the playhead's frame, with the camera shown (%s)", cut_key);
        // The button has no label of its own: "Cut" + the key cap are drawn over it.
        const float label_w = ImGui::CalcTextSize("Cut").x;
        const float key_w = ImGui::CalcTextSize(cut_key).x + 8.0f;
        const float lx = std::max(b0.x + 4.0f, b0.x + (cut_w - label_w - 6.0f - key_w) * 0.5f);
        const float ly = top + (lane_h - th) * 0.5f;
        dl->PushClipRect(b0, ImVec2(b0.x + cut_w, b0.y + lane_h), true);  // a squeezed button clips its key
        dl->AddText(ImVec2(lx, ly), can_cut ? ui::kText : ui::kFaint, "Cut");
        ui::KeyCap(dl, ImVec2(lx + label_w + 6.0f, ly - 1.5f), cut_key);
        dl->PopClipRect();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void DrawVideoPanel(float x, float y, float w, float h, const OrbitCamera& free_camera, void (*copy_log)())
{
    const VideoViewModel& m = GetVideoViewModel();
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Col(ui::kSurface));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStroke));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        // The panel never scrolls: only its body child below the
                                        // title and render rows does.
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin("##videopanel", nullptr, kFlags)) {
        ImGui::End();
        ImGui::PopStyleColor(2);
        return;
    }
    const float panel_w = ImGui::GetContentRegionAvail().x;

    // ---- Header: icon, title, close -------------------------------------------------------
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        VideoIcon(dl, ImVec2(p.x, p.y + 1.0f), IM_COL32(0xC9, 0xCE, 0xD8, 0xFF));
        ImGui::Dummy(ImVec2(18.0f, 16.0f));
        ImGui::SameLine();
        ImGui::TextUnformatted("Video");
        ImGui::SameLine(std::max(0.0f, panel_w - 18.0f));
        if (ImGui::SmallButton("x##closepanel")) SetVideoPanelVisible(false);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hide the panel (%s, or Tools > Video)", ShortcutKeyLabel(kShortcutTogglePanel));
    }
    ImGui::Spacing();

    // ---- Render: Matrix | Render, always visible (outside the scrolling body) --------------
    RenderRow(panel_w);
    ImGui::Spacing();
    ImGui::Separator();  // the pinned row above, the scrolling body below

    // ---- Scrolling body: everything below the render row ------------------------------------
    // The child fills the rest of the panel; its width shrinks while its scrollbar shows.
    ImGui::BeginChild("##videopanelbody", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    const float full = ImGui::GetContentRegionAvail().x;

    // ---- FX state on the track (UX decisions 2 and 7) -------------------------------------
    switch (m.status) {
        case VideoFxStatus::Active:   ui::StateTag(ui::kOk, "Video FX active"); break;
        case VideoFxStatus::Missing:  ui::StateTag(ui::kFaint, "No video FX on this track"); break;
        case VideoFxStatus::Bypassed: ui::StateTag(ui::kWarn, "Video FX bypassed"); break;
        case VideoFxStatus::Offline:  ui::StateTag(ui::kWarn, "Video FX offline"); break;
        case VideoFxStatus::OutOfDate: ui::StateTag(ui::kWarn, "Video FX inactive"); break;
        case VideoFxStatus::NoTrack:  ui::StateTag(ui::kFaint, "No animation displayed"); break;
    }
    if (m.status != VideoFxStatus::Active) {
        ui::SubText(StateMessage(m).body.c_str());
        FixButton(m.status, full);
        if (m.status == VideoFxStatus::OutOfDate && copy_log && ui::SolidButton("Copy error log##panel", ImVec2(full, 0.0f)))
            copy_log();
    }

    // Track: follow the displayed item, or pin one.
    if (m.track || !m.pin_choices.empty()) {
        ImGui::AlignTextToFramePadding();
        ui::Caption("Track");
        ImGui::SameLine(70.0f);
        ImGui::SetNextItemWidth(-1.0f);
        std::string preview = (m.pinned ? "Pin " RAV_DOT " " : "Follow displayed item " RAV_DOT " ") +
                              (m.track ? m.track_label : std::string("none"));
        if (ImGui::BeginCombo("##track", preview.c_str())) {
            if (ImGui::Selectable("Follow displayed item", !m.pinned)) VideoViewPin(nullptr);
            for (const VideoPinChoice& c : m.pin_choices) {
                const std::string label = "Pin " RAV_DOT " " + c.label;
                ImGui::PushID(c.track);
                if (ImGui::Selectable(label.c_str(), m.pinned && c.track == m.track)) VideoViewPin(c.track);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    // ---- Shot inspector: the shot under the playhead (UX decision 5) ----------------------
    if (m.fx >= 0 && !m.shots.empty()) {
        ImGui::Spacing();
        const int idx = std::max(0, std::min(VideoViewShotIndex(), static_cast<int>(m.shots.size()) - 1));
        const VideoShot& shot = m.shots[static_cast<size_t>(idx)];
        const bool can_edit = VideoViewCanEdit();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::Col(ui::kRaised));
        ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStroke));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::Col(ui::kBg));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
        if (ImGui::BeginChild("##inspector", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
            if (!can_edit) ImGui::BeginDisabled();

            // Head: colour dot, name, start time.
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float fh = ImGui::GetFrameHeight();
                dl->AddCircleFilled(ImVec2(p.x + 4.0f, p.y + fh * 0.5f), 4.0f, ui::kCurvePalette[idx % 8], 12);
                ImGui::Dummy(ImVec2(10.0f, fh));
                ImGui::SameLine();

                static char s_name[160] = {};
                static bool s_editing = false;
                // The shot being renamed: the one shown when the edit began (the playhead
                // may move to another shot before the field lets go).
                static MediaTrack* s_track = nullptr;
                static int s_fx = -1;
                static double s_time = 0.0;
                if (!s_editing) {
                    std::snprintf(s_name, sizeof(s_name), "%s", shot.name.c_str());
                }
                char tm[32];
                FormatTime(VideoShotFirstFrameTime(shot.time, m.fps), tm, sizeof(tm));
                const float tag_w = ImGui::CalcTextSize(tm).x + 18.0f;
                ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - tag_w - 8.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::InputText("##shotname", s_name, sizeof(s_name));
                ImGui::PopStyleColor();
                if (ImGui::IsItemActivated()) {
                    s_track = m.track;
                    s_fx = m.fx;
                    s_time = shot.time;
                }
                s_editing = ImGui::IsItemActive();
                if (ImGui::IsItemDeactivatedAfterEdit()) QueueVideoRename(s_track, s_fx, s_time, s_name);
                if (ImGui::IsItemHovered() && !s_editing) ImGui::SetTooltip("Shot name (empty = automatic)");
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ui::Caption(tm);
            }

            // Cut to next / Move to next.
            {
                static const char* const kTransitions[2] = {"Cut to next", "Move to next"};
                int sel = shot.move_to_next ? 1 : 0;
                const bool last = (idx + 1 >= static_cast<int>(m.shots.size()));
                const float item_w = std::floor((ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f);
                if (ui::Segmented("##transition", kTransitions, 2, &sel, item_w, last && sel == 0 ? 1 : -1))
                    QueueVideoTransition(sel == 1);
                if (last && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("The last shot has no next shot to move to");
            }

            // The six camera values (display units), live in the view while dragged.
            double values[vcam::kParamCount];
            if (!VideoViewInspectorValues(values)) {
                for (int p = 0; p < vcam::kParamCount; ++p) values[p] = shot.values[p];
            }
            for (int p = 0; p < vcam::kParamCount; ++p) ParamRow(p, values, can_edit);

            const float half = std::floor((ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f);
            if (ui::SolidButton("Copy RAV view", ImVec2(half, 0.0f))) QueueVideoCopyRavView(free_camera);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("This shot takes the RAV view's camera");
            ImGui::SameLine(0.0f, 6.0f);
            if (ui::SolidButton("Frame model", ImVec2(half, 0.0f))) QueueVideoFrameModel();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the angle, frame the whole model");

            if (!can_edit) ImGui::EndDisabled();
            if (!can_edit && m.status == VideoFxStatus::Active) ui::SubText("No item on this track at the playhead.");
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    }

    // ---- Story 11-5: shots, saved angles, output --------------------------------------------
    if (m.fx >= 0 && !m.shots.empty()) ShotList(m, full);
    if (m.fx >= 0) SavedAngles(m, full);
    if (m.fx >= 0 && m.track) OutputSection(m);
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleColor(2);
}

}  // namespace rav

#endif  // _WIN32
