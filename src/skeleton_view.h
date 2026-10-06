// SPDX-License-Identifier: MIT
//
// Spec 10-3c -- the 3D view's Model / Skeleton switch (RAV view and Tagging view, never Video
// view), the Dear ImGui side. In Skeleton mode the model fades (Renderer::SetModelAlpha) and
// the skeleton is drawn over it on ImGui's background draw list: bone lines and joint dots,
// projected by the CPU from the pose the renderer just drew with (no second ComputePose, no
// GL line pass). Hovering a joint names it and its roles; a click selects it (again:
// deselects). In Tagging view the bones the selected rule reads take the rule's colour. The
// Tagging panel's "+" beside a condition's Bone menu puts the selected bone into it.
//
// The mode and the selection are viewer state: never saved, Model at start. The selection is
// kept by bone name across items and dropped when the loaded skeleton has no bone of that name.

#pragma once

#ifdef _WIN32

#include <string>

namespace rav {

class Renderer;

// The model's alpha in Skeleton mode.
constexpr float kSkeletonModelAlpha = 0.18f;

bool SkeletonModeOn();
void SetSkeletonModeOn(bool on);

// The selected bone's raw name ("" = none).
const std::string& SkeletonSelectedBone();

// Once per frame, in every view, before the switch and the overlay: drops the selection when
// the loaded model (renderer's asset) has no bone of that name.
void SkeletonViewFrame(const Renderer& r);

// The Model / Skeleton switch, its bottom-left corner at (left_x, bottom_y).
void DrawSkeletonSwitch(float left_x, float bottom_y);

// The overlay over the 3D image (area_w x area_h from the window's top-left: the frame the
// renderer just drew), and the pick. Does nothing in Model mode. The hint ends at hint_right
// (left of the ViewCube), its baseline above hint_bottom.
void DrawSkeletonOverlay(const Renderer& r, float area_w, float area_h, float hint_right, float hint_bottom);

}  // namespace rav

#endif  // _WIN32
