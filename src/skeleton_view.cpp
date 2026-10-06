// SPDX-License-Identifier: MIT
//
// See skeleton_view.h.

#include "skeleton_view.h"

#ifdef _WIN32

#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "bone_roles.h"
#include "renderer.h"
#include "rule_record.h"
#include "skeleton_overlay.h"
#include "tagging_session.h"
#include "tagging_view_ui.h"
#include "ui_theme.h"
#include "video_view.h"  // TaggingViewActive

namespace rav {
namespace {

// Strings outside ASCII are spelled as UTF-8 escapes (the build has no /utf-8).
#define RAV_DOT "\xC2\xB7"  // middle dot

constexpr float kPickRadius = 10.0f;
constexpr ImU32 kKnob = IM_COL32(0xC9, 0xCE, 0xD8, 0xFF);      // the mock-up's --knob grey
constexpr ImU32 kKnobLine = IM_COL32(0xC9, 0xCE, 0xD8, 0x8C);  // its bone lines (.55)
constexpr ImU32 kLabelFill = IM_COL32(0x20, 0x23, 0x2A, 0xF2);  // kRaised at .95

constexpr ImGuiWindowFlags kSwitchFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
    ImGuiWindowFlags_NoBackground;

bool        g_skeleton = false;  // Skeleton mode (Model at start)
std::string g_selected;          // the selected bone's raw name ("" = none)

// Per-frame scratch (kept to avoid reallocating each frame).
std::vector<OverlayPoint>        g_pts;
std::vector<char>                g_vis;
std::vector<char>                g_used;
std::vector<int>                 g_rule_bones;
std::vector<int>                 g_parents;
std::vector<int>                 g_parents_now;
std::vector<std::pair<int, int>> g_segs;  // built for the skeleton whose parents are g_parents

int BoneIndexOf(const SceneSkeleton& sk, const std::string& name)
{
    if (name.empty()) return -1;
    for (size_t i = 0; i < sk.bones.size(); ++i)
        if (sk.bones[i].name == name) return static_cast<int>(i);
    return -1;
}

// The Tagging model describes this skeleton (same bones, same order): its roles and the
// selected rule's bones can be shown on it.
bool ModelMatches(const TaggingModel& m, const SceneSkeleton& sk)
{
    if (!m.file_loaded || m.bone_names.size() != sk.bones.size()) return false;
    for (size_t i = 0; i < sk.bones.size(); ++i)
        if (m.bone_names[i] != sk.bones[i].name) return false;
    return true;
}

// "mixamorig:LeftFoot · L heel" (each role mapped to that bone, built-in then custom).
std::string JointLabel(const TaggingModel* m, const std::string& name, int bone)
{
    std::string s = name;
    if (!m) return s;
    for (size_t r = 0; r < m->role_to_bone.size() && r < static_cast<size_t>(Role::Count); ++r)
        if (m->role_to_bone[r] == bone) s += " " RAV_DOT " " + RoleShortLabel(static_cast<Role>(r));
    for (const auto& kv : m->custom_role_to_bone)
        if (kv.second == bone) s += " " RAV_DOT " " + CustomRoleName(kv.first);
    return s;
}

}  // namespace

bool SkeletonModeOn()
{
    return g_skeleton;
}

void SetSkeletonModeOn(bool on)
{
    g_skeleton = on;
}

const std::string& SkeletonSelectedBone()
{
    return g_selected;
}

void SkeletonViewFrame(const Renderer& r)
{
    if (g_selected.empty()) return;
    const Asset& a = r.CurrentAsset();
    if (a.meshes.empty() && a.skeleton.bones.empty()) return;  // nothing loaded: keep it
    if (BoneIndexOf(a.skeleton, g_selected) < 0) g_selected.clear();
}

void DrawSkeletonSwitch(float left_x, float bottom_y)
{
    ImGui::SetNextWindowPos(ImVec2(left_x, bottom_y), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("##skeletonswitch", nullptr, kSwitchFlags)) {
        static const char* const kModes[2] = {"Model", "Skeleton"};
        int sel = g_skeleton ? 1 : 0;
        if (ui::Segmented("##skelmode", kModes, 2, &sel)) g_skeleton = sel == 1;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(TaggingViewActive()
                                  ? "Skeleton: see the bones, click one to select it (+ in a condition uses it)"
                                  : "Skeleton: see the bones, click one to select it");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawSkeletonOverlay(const Renderer& r, float area_w, float area_h, float hint_right, float hint_bottom)
{
    if (!g_skeleton) return;
    const Asset&                  a = r.CurrentAsset();
    const SceneSkeleton&          sk = a.skeleton;
    const std::vector<glm::mat4>* global = r.LastPoseGlobals();
    if (!global || global->size() != sk.bones.size() || sk.bones.empty()) {
        // Nothing posed (a static mesh, a rig without a clip): say why no bones show. A file with
        // no mesh draws nothing, as before.
        if (!a.meshes.empty()) {
            const char*  t = "No animated skeleton in this file";
            const ImVec2 ts = ImGui::CalcTextSize(t);
            ImGui::GetBackgroundDrawList()->AddText(ImVec2(std::max(4.0f, hint_right - ts.x), hint_bottom - ts.y),
                                                    ui::kMuted, t);
        }
        return;
    }
    const float fw = static_cast<float>(r.LastFrameWidth());
    const float fh = static_cast<float>(r.LastFrameHeight());
    if (fw < 1.0f || fh < 1.0f) return;

    // Joints projected with the frame's view-projection (top-left origin, y down).
    const size_t n = sk.bones.size();
    const float* vp = glm::value_ptr(r.LastViewProj());
    g_pts.assign(n, OverlayPoint{});
    g_vis.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const glm::vec4 p = a.modelRoot * (*global)[i][3];
        g_vis[i] = ProjectToPixels(vp, OverlayVec3{p.x, p.y, p.z}, fw, fh, &g_pts[i]) ? 1 : 0;
    }
    g_parents_now.resize(n);
    for (size_t i = 0; i < n; ++i) g_parents_now[i] = sk.bones[i].parentIdx;
    if (g_parents_now != g_parents) {  // another skeleton: its lines
        g_parents = g_parents_now;
        g_segs = BoneSegments(g_parents);
    }

    // Roles and the selected rule's bones come from the Tagging model when it describes this
    // skeleton (else the label shows the name only).
    const TaggingModel& tm = GetTaggingModel();
    const TaggingModel* m = ModelMatches(tm, sk) ? &tm : nullptr;
    g_used.assign(n, 0);
    ImU32 rule_col = kKnob;
    if (m && TaggingViewActive()) {
        unsigned int c = 0;
        if (TaggingSelectedRuleBones(&g_rule_bones, &c)) {
            rule_col = c;
            for (int b : g_rule_bones)
                if (b >= 0 && static_cast<size_t>(b) < n) g_used[static_cast<size_t>(b)] = 1;
        }
    }

    // Hover and pick: only when ImGui does not want the mouse (panels, strip, overlays keep
    // their clicks) and the mouse is over the 3D view.
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2   mouse = io.MousePos;
    int hovered = -1;
    if (!io.WantCaptureMouse && mouse.x >= 0.0f && mouse.y >= 0.0f && mouse.x < area_w && mouse.y < area_h) {
        hovered = NearestJoint(g_pts, g_vis, OverlayPoint{mouse.x, mouse.y}, kPickRadius);
        if (hovered >= 0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                const std::string& name = sk.bones[static_cast<size_t>(hovered)].name;
                g_selected = (g_selected == name) ? std::string() : name;
            }
        }
    }
    const int selected = BoneIndexOf(sk, g_selected);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(area_w, area_h), false);
    // Bone lines: the rule's colour between two of its bones, else the neutral grey.
    for (const auto& s : g_segs) {
        const size_t c = static_cast<size_t>(s.first), p = static_cast<size_t>(s.second);
        if (!g_vis[c] || !g_vis[p]) continue;
        const ImU32 col = (g_used[c] && g_used[p]) ? rule_col : kKnobLine;
        dl->AddLine(ImVec2(g_pts[c].x, g_pts[c].y), ImVec2(g_pts[p].x, g_pts[p].y), col, 2.0f);
    }
    // Joints: the selected one blue, the rule's bones in its colour, the others grey; the
    // selected and hovered ones drawn last (on top), larger, with a white ring.
    auto joint = [&](size_t i) {
        const bool  sel = static_cast<int>(i) == selected;
        const bool  hov = static_cast<int>(i) == hovered;
        const ImU32 col = sel ? ui::kAccent : g_used[i] ? rule_col : kKnob;
        const ImVec2 c(g_pts[i].x, g_pts[i].y);
        dl->AddCircleFilled(c, (sel || hov) ? 6.0f : 4.0f, col, 16);
        if (sel || hov) dl->AddCircle(c, 6.0f, ui::kWhite, 16, 1.5f);
    };
    for (size_t i = 0; i < n; ++i)
        if (g_vis[i] && static_cast<int>(i) != selected && static_cast<int>(i) != hovered) joint(i);
    if (selected >= 0 && g_vis[static_cast<size_t>(selected)]) joint(static_cast<size_t>(selected));
    if (hovered >= 0 && hovered != selected) joint(static_cast<size_t>(hovered));

    // The label: the hovered joint, else the selected one (with "selected").
    const int lab = hovered >= 0 ? hovered : selected;
    if (lab >= 0 && g_vis[static_cast<size_t>(lab)]) {
        std::string txt = JointLabel(m, sk.bones[static_cast<size_t>(lab)].name, lab);
        if (hovered < 0) txt += " " RAV_DOT " selected";
        const ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
        const float  w = ts.x + 14.0f, h = ts.y + 6.0f;
        float x = g_pts[static_cast<size_t>(lab)].x + 10.0f;
        float y = g_pts[static_cast<size_t>(lab)].y - h - 4.0f;
        x = std::max(4.0f, std::min(x, area_w - w - 4.0f));
        y = std::max(4.0f, std::min(y, area_h - h - 4.0f));
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), kLabelFill, 3.0f);
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ui::kStrokeStrong, 3.0f);
        dl->AddText(ImVec2(x + 7.0f, y + 3.0f), ui::kText, txt.c_str());
    }
    dl->PopClipRect();

    // The hint, bottom right (left of the ViewCube).
    const std::string hint = g_selected.empty()   ? std::string("Click a bone to select it")
                             : TaggingViewActive() ? g_selected + " selected " RAV_DOT " + in a condition uses it"
                                                   : g_selected + " selected";
    const ImVec2 hs = ImGui::CalcTextSize(hint.c_str());
    dl->AddText(ImVec2(std::max(4.0f, hint_right - hs.x), hint_bottom - hs.y), ui::kMuted, hint.c_str());
}

}  // namespace rav

#endif  // _WIN32
