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
#include "ui_theme.h"

namespace rav {
namespace {

// DECISIONS.md section 1: default widths 16 / 22 / 32 / 30 %, minimum 180 / 220 / 300 / 280 px.
constexpr float kColumnShare[4] = {0.16f, 0.22f, 0.32f, 0.30f};
constexpr float kColumnMin[4] = {180.0f, 220.0f, 300.0f, 280.0f};
constexpr float kColumnGap = 6.0f;

void DrawColumn(int i)
{
    switch (i) {
    case 0:
        DrawLibrariesSection();
        DrawUserLibrarySection();
        DrawSwitchGroupsSection();
        break;
    case 1: DrawLibraryTree(); break;
    case 2:
        DrawNodePanel();
        DrawUserLibraryNodeSection();
        break;
    default: DrawMediaExplorerPanel(); break;
    }
}

}  // namespace

void DrawLibraryView(float x, float y, float w, float h)
{
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
        const float avail = std::max(1.0f, w - 3.0f * kColumnGap);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::Col(ui::kSurface));
        ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::kStroke));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui::kRadiusMd);
        static const char* const kIds[4] = {"##libcol1", "##libcol2", "##libcol3", "##libcol4"};
        for (int i = 0; i < 4; ++i) {
            if (i > 0) ImGui::SameLine(0.0f, kColumnGap);
            const float cw = std::max(kColumnMin[i], std::floor(avail * kColumnShare[i]));
            if (ImGui::BeginChild(kIds[i], ImVec2(cw, std::max(1.0f, h - 2.0f)), ImGuiChildFlags_Borders))
                DrawColumn(i);
            ImGui::EndChild();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace rav

#endif  // _WIN32
