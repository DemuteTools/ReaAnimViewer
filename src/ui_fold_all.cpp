// SPDX-License-Identifier: MIT
//
// See ui_fold_all.h.

#include "ui_fold_all.h"

#ifdef _WIN32

#include <algorithm>
#include <map>

#include <imgui_internal.h>  // ImHashStr

#include "library_tree_nav.h"  // FoldScopeButtonsFor
#include "ui_theme.h"

namespace rav {
namespace ui {
namespace {

struct FoldScope {
    int count = 0, open = 0;            // this frame's foldable sections, and how many are open
    int last_count = 0, last_open = 0;  // last frame's
    int pending = 0;                    // +1 expand / -1 collapse, asked by the bar
    int applying = 0;                   // the pending request, applied while this frame's scope is drawn
};
std::map<ImGuiID, FoldScope> g_scopes;
FoldScope*                   g_current = nullptr;

FoldScope& ScopeOf(const char* id) { return g_scopes[ImHashStr(id ? id : "")]; }

}  // namespace

int FoldAllButtons(const char* id, bool can_expand, bool can_collapse, const char* off_tip)
{
    int r = 0;
    ImGui::PushID(id);
    ImGui::BeginDisabled(!can_expand);
    if (IconOnlyButton("##expandall", "\xE2\x8A\x9E", off_tip ? off_tip : "Expand all")) r = 1;
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, Dp(2.0f));
    ImGui::BeginDisabled(!can_collapse);
    if (IconOnlyButton("##collapseall", "\xE2\x8A\x9F", off_tip ? off_tip : "Collapse all")) r = -1;
    ImGui::EndDisabled();
    ImGui::PopID();
    return r;
}

float FoldAllButtonsWidth() { return 2.0f * ControlH() + Dp(2.0f); }

void BeginFoldScope(const char* id)
{
    FoldScope& s = ScopeOf(id);
    s.count = s.open = 0;
    s.applying = s.pending;
    s.pending = 0;
    g_current = &s;
}

void EndFoldScope(bool complete)
{
    if (!g_current) return;
    if (complete) {  // a frame cut short by a gesture keeps the last full count (the bar never blinks)
        g_current->last_count = g_current->count;
        g_current->last_open = g_current->open;
    } else if (g_current->applying != 0) {
        g_current->pending = g_current->applying;  // the sections not drawn get it next frame
    }
    g_current->applying = 0;
    g_current = nullptr;
}

bool FoldScopeBar(const char* id)
{
    FoldScope& s = ScopeOf(id);
    const FoldScopeButtons b = FoldScopeButtonsFor(s.last_count, s.last_open);
    if (!b.shown) return false;
    const float  w = FoldAllButtonsWidth();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x + std::max(0.0f, ImGui::GetContentRegionAvail().x - w), p.y));
    const int r = FoldAllButtons(id, b.can_expand, b.can_collapse);
    if (r != 0) s.pending = r;
    return true;
}

void NoteFoldableSection(ImGuiID open_id, bool* open)
{
    if (!g_current || !open) return;
    if (g_current->applying != 0) {
        *open = g_current->applying > 0;
        ImGui::GetStateStorage()->SetBool(open_id, *open);
    }
    ++g_current->count;
    if (*open) ++g_current->open;
}

}  // namespace ui
}  // namespace rav

#endif  // _WIN32
