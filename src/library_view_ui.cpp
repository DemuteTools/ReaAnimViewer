// SPDX-License-Identifier: MIT
//
// See library_view_ui.h.

#include "library_view_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "library_media_ui.h"
#include "library_node_ui.h"
#include "library_tree_ui.h"
#include "library_user_ui.h"
#include "reaper_api.h"
#include "shortcuts.h"
#include "sound_project.h"
#include "sound_record.h"
#include "ui_theme.h"
#include "undo_guard.h"

namespace rav {
namespace {

// DECISIONS.md section 1: default widths 16 / 22 / 32 / 30 %, minimum 180 / 220 / 300 / 280 px.
constexpr float kColumnShare[4] = {0.16f, 0.22f, 0.32f, 0.30f};
constexpr float kColumnMin[4] = {180.0f, 220.0f, 300.0f, 280.0f};
constexpr float kColumnGap = 6.0f;
constexpr float kStripWidth = 28.0f;
constexpr double kToastSeconds = 5.0;
constexpr const char* kWidthKeys[4] = {"lib_col_w0", "lib_col_w1", "lib_col_w2", "lib_col_w3"};

struct ViewState {
    bool  loaded = false;
    float share[4] = {kColumnShare[0], kColumnShare[1], kColumnShare[2], kColumnShare[3]};
    bool  col1_collapsed = false;
    bool  media_collapsed = false;
    bool  focused = false;
};
ViewState g_view;

struct Toast {
    std::string text;
    bool        undo = false;
    std::string undo_desc;   // the gesture's undo point description
    int         state = -1;  // the project state right after the gesture (read when the toast is shown)
    double      until = 0.0;
};
Toast g_toast;

void LoadPrefs()
{
    if (g_view.loaded) return;
    g_view.loaded = true;
    for (int i = 0; i < 4; ++i) {
        const float v = LoadPrefFloat(kWidthKeys[i], kColumnShare[i]);
        g_view.share[i] = (v > 0.01f && v < 0.95f) ? v : kColumnShare[i];
    }
    g_view.col1_collapsed = LoadPrefBool("lib_col1_collapsed", false);
    g_view.media_collapsed = LoadPrefBool("lib_media_collapsed", false);
}

void SaveWidths()
{
    for (int i = 0; i < 4; ++i) SavePrefFloat(kWidthKeys[i], g_view.share[i]);
}

void ResetWidths()
{
    for (int i = 0; i < 4; ++i) g_view.share[i] = kColumnShare[i];
    SaveWidths();
}

bool Collapsed(int i) { return (i == 0 && g_view.col1_collapsed) || (i == 3 && g_view.media_collapsed); }

void DrawColumn(int i)
{
    switch (i) {
    case 0:
        if (ImGui::SmallButton("\xC2\xAB##collapse1")) SetLibraryColumn1Collapsed(true);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collapse (Ctrl+B)");
        DrawLibrariesSection();
        DrawUserLibrarySection();
        DrawSwitchGroupsSection();
        break;
    case 1: DrawLibraryTree(); break;
    case 2:
        if (SelectedLibraryRows().size() > 1) {
            DrawLibraryBatchPanel();
        } else {
            DrawNodePanel();
            DrawUserLibraryNodeSection();
        }
        break;
    default:
        if (ImGui::SmallButton("\xC2\xBB##collapse4")) {
            g_view.media_collapsed = true;
            SavePrefBool("lib_media_collapsed", true);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collapse the Media Explorer");
        DrawMediaExplorerPanel();
        break;
    }
}

// A collapsed column: a 28 px strip with its expand button and its name, one letter per line.
void DrawStrip(int i, float h)
{
    ImGui::PushID(i);
    if (ImGui::SmallButton(i == 0 ? "\xC2\xBB" : "\xC2\xAB")) {
        if (i == 0) SetLibraryColumn1Collapsed(false);
        else {
            g_view.media_collapsed = false;
            SavePrefBool("lib_media_collapsed", false);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(i == 0 ? "Show the libraries (Ctrl+B)" : "Show the Media Explorer");
    std::string name = "Media";
    ImU32 dot = ui::kAccent;
    if (i == 0) {
        name.clear();
        if (const SoundLibrary* lib = FindLibrary(GetSoundStore(nullptr), SelectedLibraryUid())) {
            name = lib->name;
            if (lib->color & 0x1000000u)
                dot = IM_COL32((lib->color >> 16) & 0xFF, (lib->color >> 8) & 0xFF, lib->color & 0xFF, 0xFF);
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float cx = p.x + kStripWidth * 0.5f - 4.0f;
    if (i == 0) {
        dl->AddCircleFilled(ImVec2(cx + 3.0f, p.y + 8.0f), 4.0f, dot);
        p.y += 18.0f;
    }
    const float lh = ImGui::GetTextLineHeight();
    // One UTF-8 character per line (an accented letter is several bytes).
    for (size_t k = 0; k < name.size() && p.y + lh < ImGui::GetWindowPos().y + h;) {
        const unsigned char c = static_cast<unsigned char>(name[k]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        len = std::min(len, name.size() - k);
        dl->AddText(ImVec2(cx, p.y), ui::kMuted, name.data() + k, name.data() + k + len);
        k += len;
        p.y += lh;
    }
    ImGui::PopID();
}

// The splitter between columns a and b: accent on hover, drag to resize, double-click resets.
void Splitter(int a, int b, float* px, float h, float visible_px, float visible_share)
{
    ImGui::PushID(a * 8 + b);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##split", ImVec2(kColumnGap, std::max(1.0f, h)));
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + 2.0f, p.y), ImVec2(p.x + kColumnGap - 2.0f, p.y + h),
                                              hot ? ui::kAccent : ui::kBg);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        ResetWidths();
    } else if (ImGui::IsItemActive()) {
        const float dx = ImGui::GetIO().MouseDelta.x;
        if (dx != 0.0f) {
            float na = px[a] + dx, nb = px[b] - dx;
            if (na < kColumnMin[a]) {
                nb -= kColumnMin[a] - na;
                na = kColumnMin[a];
            }
            if (nb < kColumnMin[b]) {
                na -= kColumnMin[b] - nb;
                nb = kColumnMin[b];
            }
            if (na >= kColumnMin[a] && nb >= kColumnMin[b] && visible_px > 1.0f) {
                g_view.share[a] = na / visible_px * visible_share;
                g_view.share[b] = nb / visible_px * visible_share;
                px[a] = na;
                px[b] = nb;
            }
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit() || ImGui::IsItemDeactivated()) SaveWidths();
    ImGui::PopID();
}

void DrawToast()
{
    if (g_toast.text.empty()) return;
    if (ImGui::GetTime() > g_toast.until) {
        g_toast.text.clear();
        return;
    }
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y - 24.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Col(ui::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStrokeStrong));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::kRadiusMd);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##libtoast", nullptr, kFlags)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(g_toast.text.c_str());
        // Any later change (another edit, an undo) retires the Undo: it would undo that instead.
        if (g_toast.undo) {
            const int state = GetProjectStateChangeCount(nullptr);
            if (g_toast.state < 0) g_toast.state = state;
            g_toast.undo = GestureUndoAvailable(Undo_CanUndo2(nullptr), g_toast.undo_desc, state, g_toast.state);
        }
        if (g_toast.undo) {
            ImGui::SameLine();
            if (ui::SolidButton("Undo")) {
                Main_OnCommand(40029, 0);  // Edit: Undo
                g_toast.text.clear();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

}  // namespace

void ShowLibraryToast(const std::string& text, bool undo, const std::string& undo_desc)
{
    g_toast.text = text;
    g_toast.undo = undo;
    g_toast.undo_desc.clear();
    if (undo) {
        const char* last = Undo_CanUndo2(nullptr);
        g_toast.undo_desc = !undo_desc.empty() ? undo_desc : (last ? last : "");
    }
    // Every caller shows its toast once the gesture's undo point is closed: read the state now,
    // so an undo made while the view is hidden cannot slip in before the first draw.
    g_toast.state = undo ? GetProjectStateChangeCount(nullptr) : -1;
    g_toast.until = ImGui::GetTime() + kToastSeconds;
}

bool LibraryViewFocused() { return g_view.focused; }

bool LibraryViewTakesKey(unsigned vk, bool ctrl, bool shift, bool alt)
{
    if (LibraryNodeTakesKey(vk, ctrl, shift, alt)) return true;  // 13-7b: E, S (and the Edit overlay's keys)
    if (!g_view.focused || alt) return false;
    constexpr unsigned kDelete = 0x2E, kF2 = 0x71, kSpace = 0x20, kEscape = 0x1B;
    if (!ctrl && !shift && (vk == 0x26 || vk == 0x28) && !g_view.media_collapsed && MediaPanelTakesArrows())
        return true;  // 13-7c: Up/Down move the Media Explorer's row
    if (ctrl) return !shift && (vk == 'B' || vk == 'D' || vk == 'X' || vk == 'V');
    return vk == kDelete || vk == kF2 || vk == kSpace || vk == kEscape;
}

bool LibraryColumn1Collapsed()
{
    LoadPrefs();
    return g_view.col1_collapsed;
}

void SetLibraryColumn1Collapsed(bool collapsed)
{
    LoadPrefs();
    if (g_view.col1_collapsed == collapsed) return;
    g_view.col1_collapsed = collapsed;
    SavePrefBool("lib_col1_collapsed", collapsed);
}

void SetLibraryMediaCollapsed(bool collapsed)
{
    LoadPrefs();
    if (g_view.media_collapsed == collapsed) return;
    g_view.media_collapsed = collapsed;
    SavePrefBool("lib_media_collapsed", collapsed);
}

void DrawLibraryView(float x, float y, float w, float h)
{
    LoadPrefs();
    // The 3D view stays hidden behind the Library view (its gaps and margins included).
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, ui::kBg);
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(std::max(1.0f, w), std::max(1.0f, h)), ImGuiCond_Always);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_HorizontalScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("##libraryview", nullptr, kFlags)) {
        g_view.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const ImGuiIO& io = ImGui::GetIO();
        if (g_view.focused && !io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, false))
            SetLibraryColumn1Collapsed(!g_view.col1_collapsed);

        // Widths: collapsed columns take a strip; the others share the rest by their shares.
        float strips = 0.0f, visible_share = 0.0f;
        for (int i = 0; i < 4; ++i) {
            if (Collapsed(i)) strips += kStripWidth;
            else visible_share += g_view.share[i];
        }
        const float visible_px = std::max(1.0f, w - 3.0f * kColumnGap - strips);
        float px[4];
        for (int i = 0; i < 4; ++i)
            px[i] = Collapsed(i) ? kStripWidth
                                 : std::max(kColumnMin[i], std::floor(visible_px * g_view.share[i] /
                                                                      std::max(0.01f, visible_share)));

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::Col(ui::kSurface));
        ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStroke));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui::kRadiusMd);
        static const char* const kIds[4] = {"##libcol1", "##libcol2", "##libcol3", "##libcol4"};
        const float ch = std::max(1.0f, h - 2.0f);
        for (int i = 0; i < 4; ++i) {
            if (i > 0) {
                ImGui::SameLine(0.0f, 0.0f);
                if (!Collapsed(i - 1) && !Collapsed(i)) Splitter(i - 1, i, px, ch, visible_px, visible_share);
                else ImGui::Dummy(ImVec2(kColumnGap, ch));
                ImGui::SameLine(0.0f, 0.0f);
            }
            if (ImGui::BeginChild(kIds[i], ImVec2(px[i], ch), ImGuiChildFlags_Borders)) {
                if (Collapsed(i)) DrawStrip(i, ch);
                else DrawColumn(i);
            }
            ImGui::EndChild();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    DrawToast();
}

}  // namespace rav

#endif  // _WIN32
