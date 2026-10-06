// SPDX-License-Identifier: MIT
//
// Spec 10-3c -- the pure geometry of the 3D view's skeleton overlay (Model / Skeleton switch):
// a joint projected to window pixels, the joint nearest the mouse, the bone segments.
// Header-only, no GL, no ImGui, no glm (the matrix is 16 floats, column-major, as
// glm::value_ptr gives it), so the host tests build it anywhere.
//   c++ -std=c++17 -I src tests/skeleton_overlay_test.cpp -o t && ./t

#pragma once

#include <utility>
#include <vector>

namespace rav {

struct OverlayVec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct OverlayPoint {
    float x = 0.0f, y = 0.0f;
};

// Projects the world point p through view_proj (column-major 4x4) into a w x h pixel area
// whose origin is the top-left corner (y down). False when the point is at or behind the
// camera (clip w <= 0): it is then neither drawn nor pickable. A point outside the frustum
// sideways still projects (its pixel lies outside the area).
inline bool ProjectToPixels(const float* view_proj, const OverlayVec3& p, float w, float h, OverlayPoint* out)
{
    const float* m = view_proj;
    const float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
    const float cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    const float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (!(cw > 0.0f)) return false;
    const float nx = cx / cw;
    const float ny = cy / cw;
    if (out) {
        out->x = (nx * 0.5f + 0.5f) * w;
        out->y = (1.0f - (ny * 0.5f + 0.5f)) * h;
    }
    return true;
}

// The index of the point nearest `mouse` within `radius` pixels (inclusive), -1 when none.
// `visible` (same size as points, or empty = all visible) skips the joints not drawn. On a
// tie the first index wins.
inline int NearestJoint(const std::vector<OverlayPoint>& points, const std::vector<char>& visible,
                        OverlayPoint mouse, float radius)
{
    int   best = -1;
    float best_d2 = radius * radius;
    for (size_t i = 0; i < points.size(); ++i) {
        if (!visible.empty() && (i >= visible.size() || !visible[i])) continue;
        const float dx = points[i].x - mouse.x;
        const float dy = points[i].y - mouse.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best_d2 || (best < 0 && d2 <= best_d2)) {
            best = static_cast<int>(i);
            best_d2 = d2;
        }
    }
    return best;
}

// The bone lines: one (child, parent) pair per bone with a parent in range (a root has none).
inline std::vector<std::pair<int, int>> BoneSegments(const std::vector<int>& parents)
{
    std::vector<std::pair<int, int>> out;
    const int n = static_cast<int>(parents.size());
    for (int i = 0; i < n; ++i) {
        const int p = parents[static_cast<size_t>(i)];
        if (p >= 0 && p < n && p != i) out.emplace_back(i, p);
    }
    return out;
}

}  // namespace rav
