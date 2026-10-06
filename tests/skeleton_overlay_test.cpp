// SPDX-License-Identifier: MIT
//
// Host test of the skeleton overlay's geometry (src/skeleton_overlay.h, spec 10-3c): the
// projection to pixels (behind the camera = not drawn), the nearest joint (radius edge, ties,
// hidden joints) and the bone segments (roots have none). No REAPER, no Windows, no GL.
//   c++ -std=c++17 -I src tests/skeleton_overlay_test.cpp -o t && ./t

#include "skeleton_overlay.h"

#include <cmath>
#include <cstdio>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-3f;
}

// Column-major identity, and a minimal perspective (w = -z: the camera looks down -Z).
void Identity(float* m)
{
    for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}
void Perspective(float* m)
{
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = 1.0f;    // x
    m[5] = 1.0f;    // y
    m[10] = -1.0f;  // z (unused here)
    m[11] = -1.0f;  // w = -z
    m[14] = -0.1f;
}

}  // namespace

int main()
{
    // ---- ProjectToPixels ----
    {
        float m[16];
        Identity(m);
        OverlayPoint p;
        // The centre of NDC lands in the middle of the area, top-left origin, y down.
        CHECK(ProjectToPixels(m, OverlayVec3{0.0f, 0.0f, 0.0f}, 200.0f, 100.0f, &p));
        CHECK(Near(p.x, 100.0f) && Near(p.y, 50.0f));
        CHECK(ProjectToPixels(m, OverlayVec3{-1.0f, 1.0f, 0.0f}, 200.0f, 100.0f, &p));
        CHECK(Near(p.x, 0.0f) && Near(p.y, 0.0f));  // NDC top-left = pixel (0, 0)
        CHECK(ProjectToPixels(m, OverlayVec3{1.0f, -1.0f, 0.0f}, 200.0f, 100.0f, &p));
        CHECK(Near(p.x, 200.0f) && Near(p.y, 100.0f));
        // Outside the frustum sideways: still projects, outside the area.
        CHECK(ProjectToPixels(m, OverlayVec3{3.0f, 0.0f, 0.0f}, 200.0f, 100.0f, &p));
        CHECK(p.x > 200.0f);
    }
    {
        float m[16];
        Perspective(m);
        OverlayPoint p;
        // In front of the camera (z < 0): w > 0, divided by w.
        CHECK(ProjectToPixels(m, OverlayVec3{1.0f, 0.0f, -2.0f}, 100.0f, 100.0f, &p));
        CHECK(Near(p.x, 75.0f) && Near(p.y, 50.0f));
        // Behind the camera (w < 0) and on its plane (w == 0): not drawn.
        p = OverlayPoint{-7.0f, -7.0f};
        CHECK(!ProjectToPixels(m, OverlayVec3{1.0f, 0.0f, 2.0f}, 100.0f, 100.0f, &p));
        CHECK(Near(p.x, -7.0f));  // untouched
        CHECK(!ProjectToPixels(m, OverlayVec3{1.0f, 0.0f, 0.0f}, 100.0f, 100.0f, &p));
        // A null output is allowed (visibility test only).
        CHECK(ProjectToPixels(m, OverlayVec3{0.0f, 0.0f, -1.0f}, 100.0f, 100.0f, nullptr));
    }

    // ---- NearestJoint ----
    {
        const std::vector<OverlayPoint> pts = {{10.0f, 10.0f}, {30.0f, 10.0f}, {20.0f, 10.0f}, {20.0f, 10.0f}};
        // The nearest within the radius.
        CHECK(NearestJoint(pts, {}, OverlayPoint{29.0f, 11.0f}, 10.0f) == 1);
        // Nothing within the radius.
        CHECK(NearestJoint(pts, {}, OverlayPoint{100.0f, 100.0f}, 10.0f) == -1);
        CHECK(NearestJoint({}, {}, OverlayPoint{0.0f, 0.0f}, 10.0f) == -1);
        // Radius edge: exactly 10 px picks, just past it does not.
        CHECK(NearestJoint({{0.0f, 0.0f}}, {}, OverlayPoint{10.0f, 0.0f}, 10.0f) == 0);
        CHECK(NearestJoint({{0.0f, 0.0f}}, {}, OverlayPoint{10.01f, 0.0f}, 10.0f) == -1);
        CHECK(NearestJoint({{0.0f, 0.0f}}, {}, OverlayPoint{6.0f, 8.0f}, 10.0f) == 0);
        // Ties: the first index wins (two joints on the same pixel, and equidistant ones).
        CHECK(NearestJoint(pts, {}, OverlayPoint{20.0f, 10.0f}, 10.0f) == 2);
        CHECK(NearestJoint({{0.0f, 0.0f}, {10.0f, 0.0f}}, {}, OverlayPoint{5.0f, 0.0f}, 10.0f) == 0);
        CHECK(NearestJoint({{0.0f, 0.0f}, {10.0f, 0.0f}}, {}, OverlayPoint{10.0f, 0.0f}, 10.0f) == 1);
        // A hidden joint (behind the camera) is never picked.
        CHECK(NearestJoint(pts, {1, 0, 1, 1}, OverlayPoint{29.0f, 11.0f}, 10.0f) == 2);
        CHECK(NearestJoint(pts, {0, 0, 0, 0}, OverlayPoint{20.0f, 10.0f}, 10.0f) == -1);
        // A visibility list too short hides the joints it does not cover.
        CHECK(NearestJoint(pts, {1}, OverlayPoint{29.0f, 11.0f}, 10.0f) == -1);
    }

    // ---- BoneSegments ----
    {
        // hips (root) <- spine <- head; hips <- leg; a second root.
        const std::vector<int> parents = {-1, 0, 1, 0, -1};
        const auto segs = BoneSegments(parents);
        CHECK(segs.size() == 3);
        CHECK(segs.size() == 3 && segs[0] == std::make_pair(1, 0) && segs[1] == std::make_pair(2, 1) &&
              segs[2] == std::make_pair(3, 0));
        // Out-of-range or self parents make no line.
        CHECK(BoneSegments({-1, 7, 2}).empty());
        CHECK(BoneSegments({}).empty());
        CHECK(BoneSegments({-1}).empty());
    }

    if (g_fails == 0) std::printf("skeleton_overlay: all passed\n");
    return g_fails == 0 ? 0 : 1;
}
