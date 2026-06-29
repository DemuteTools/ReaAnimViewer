// SPDX-License-Identifier: MIT
//
// D14 orbit camera: a small POD-style controller whose state is {target, distance,
// yaw, pitch}. The view matrix is DERIVED from this state every frame (so dragging
// is continuous, FR26), not cached. Camera state is panel state, NOT Asset state —
// it survives a reload (D4), so it lives in the renderer, not on the Asset. Reset()
// re-frames the model's AABB and is both the auto-fit-on-load and the Reset-View
// recovery path (AR13 / Journey 2). Header-only (inline math, no .cpp) so there is
// no CMakeLists.txt change — CMake enumerates only .cpp sources.

#pragma once

#ifdef _WIN32

#include <algorithm>   // std::clamp
#include <cmath>       // std::cos/sin/exp/isfinite

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::lookAt

namespace rav {

// Sensitivity knobs — the gate-tunable "feels-right" constants (Story 2.4 Task 4
// note). They cannot be judged on the Linux dev box; tweak here if orbit/zoom/pan
// feel too fast or coarse on the Windows gate.
constexpr float kOrbitSens = 0.01f;   // radians per pixel of drag
constexpr float kZoomSens  = 0.1f;    // exponential zoom rate per wheel notch
constexpr float kPanSens   = 0.002f;  // pan distance per pixel, scaled by distance
constexpr float kInitYaw   = 0.6f;    // initial 3/4 azimuth (matches the Epic 1 cube angle)
constexpr float kInitPitch = 0.3f;    // D14 initial elevation

// Story 6.5.8 — ViewCube snap tween duration (seconds). Another gate-tunable feel knob:
// long enough to read as a smooth move, short enough to feel instant. Tweak at the gate.
constexpr float kSnapDuration = 0.2f;

struct OrbitCamera {
    glm::vec3 target{0.0f};
    float distance    = 3.0f;
    float yaw         = kInitYaw;
    float pitch       = kInitPitch;
    float frameRadius = 1.0f;  // set at Reset; the scale reference for the zoom clamp

    // Story 6.5.8 — ViewCube snap tween. SnapToDirection arms an ease-in/out interpolation
    // of {yaw, pitch} from the captured start pose to the target; AdvanceAnim steps it each
    // frame. Purely DERIVED session state — nothing here persists (the resulting yaw/pitch
    // serialize via Story 6.2 like any camera state). A live Orbit/Pan/Zoom/Reset cancels it
    // so the user's input always wins over an in-flight tween.
    bool  animating   = false;
    float animElapsed = 0.0f;
    float startYaw    = kInitYaw;
    float startPitch  = kInitPitch;
    float targetYaw   = kInitYaw;
    float targetPitch = kInitPitch;

    glm::vec3 Eye() const {
        const float cp = std::cos(pitch);
        return target + distance * glm::vec3(cp * std::sin(yaw),
                                             std::sin(pitch),
                                             cp * std::cos(yaw));
    }

    glm::mat4 ViewMatrix() const {
        return glm::lookAt(Eye(), target, glm::vec3(0.0f, 1.0f, 0.0f));  // world-up Y (AR9)
    }

    void Orbit(int dx, int dy) {
        animating = false;  // a live drag wins over an in-flight ViewCube snap (6.5.8)
        // dx is negated so a rightward drag rotates the model rightward (drag-the-
        // model feel); with +dx the horizontal motion came out reversed on the gate.
        yaw   -= dx * kOrbitSens;
        pitch += dy * kOrbitSens;
        // Clamp pitch shy of +/-pi/2: at the poles the eye aligns with world-up and
        // lookAt degenerates (gimbal flip), so never let it reach there.
        const float lim = 1.55f;  // ~pi/2 - eps
        pitch = std::clamp(pitch, -lim, lim);
    }

    void Zoom(float delta) {
        animating = false;  // a live zoom wins over an in-flight ViewCube snap (6.5.8)
        distance *= std::exp(-delta * kZoomSens);  // exponential feel (D14)
        if (!std::isfinite(distance)) distance = frameRadius * 3.0f;
        // Clamp to a scale-independent band so you can't zoom THROUGH the model to
        // 0/negative, or out to infinity and lose it.
        distance = std::clamp(distance, frameRadius * 0.05f, frameRadius * 20.0f);
    }

    void Pan(int dx, int dy) {
        animating = false;  // a live pan wins over an in-flight ViewCube snap (6.5.8)
        // Derive the view basis from the current eye so pan tracks the screen axes.
        const glm::vec3 fwd   = glm::normalize(target - Eye());
        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 up    = glm::cross(right, fwd);
        // Scale by distance so the model tracks the cursor at any zoom (D14).
        target += (-right * float(dx) + up * float(dy)) * distance * kPanSens;
    }

    // Story 6.5.8 — point the camera along `dir` (a unit direction FROM the scene centre
    // toward where the eye should sit) by solving the INVERSE of Eye()'s spherical map:
    // Eye() puts the eye at distance*(cos(pitch)sin(yaw), sin(pitch), cos(pitch)cos(yaw)),
    // so pitch = asin(dir.y) and yaw = atan2(dir.x, dir.z). ONLY {yaw, pitch} move —
    // target, distance, frameRadius are untouched (the snap changes the angle, not the
    // framing). targetPitch is clamped to the same gimbal limit as Orbit so a top/bottom
    // click lands just under the pole, never on it (lookAt degenerates there). The yaw
    // target is unwrapped to within +/-pi of the current yaw so the tween takes the
    // SHORTEST path and never spins the long way around. No angle is hardcoded: a
    // face/edge/corner direction yields straight-on / 45 / ~35 purely from its geometry.
    void SnapToDirection(const glm::vec3& dir) {
        if (!(glm::dot(dir, dir) > 1e-12f)) return;  // degenerate direction → no-op
        const glm::vec3 d = glm::normalize(dir);
        const float lim = 1.55f;  // == the Orbit pitch clamp (gimbal guard)
        const float kTwoPi = 6.28318530718f;
        float tp = std::asin(std::clamp(d.y, -1.0f, 1.0f));
        tp = std::clamp(tp, -lim, lim);
        float ty = std::atan2(d.x, d.z);
        ty = yaw + std::remainder(ty - yaw, kTwoPi);  // shortest-path unwrap into [yaw-pi, yaw+pi]
        startYaw    = yaw;
        startPitch  = pitch;
        targetYaw   = ty;
        targetPitch = tp;
        animElapsed = 0.0f;
        animating   = true;
    }

    // Story 6.5.8 — step the snap tween by dt seconds (called once per frame before the
    // view is derived). Eases the captured start->target with smoothstep for a soft
    // start/stop; lerps from the STORED start (not the live value) so it converges cleanly
    // instead of drifting. Snaps exactly to the target and disarms at t>=1. No-op (and
    // allocation-free) when not animating — the common case.
    void AdvanceAnim(float dt) {
        if (!animating) return;
        animElapsed += dt;
        float t = (kSnapDuration > 0.0f) ? animElapsed / kSnapDuration : 1.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const float e = t * t * (3.0f - 2.0f * t);  // smoothstep ease-in/out
        yaw   = startYaw   + (targetYaw   - startYaw)   * e;
        pitch = startPitch + (targetPitch - startPitch) * e;
        if (t >= 1.0f) {
            yaw       = targetYaw;
            pitch     = targetPitch;
            animating = false;
        }
    }

    void Reset(const glm::vec3& aabbMin, const glm::vec3& aabbMax) {
        animating = false;  // Recenter cancels an in-flight ViewCube snap (6.5.8)
        glm::vec3 center = 0.5f * (aabbMin + aabbMax);
        float diag = glm::length(aabbMax - aabbMin);
        // `!(... > eps)` (not `... < eps`) so a NaN — which compares false against
        // everything — also hits the fallback instead of producing a NaN camera
        // (mirrors SetAsset's degenerate-bounds guard). isfinite also rejects an
        // infinite diagonal (a corrupt asset with an inf vertex): inf > eps is true,
        // so without it the fallback is skipped and frameRadius/distance go infinite.
        if (!(std::isfinite(diag) && diag > 1e-4f)) diag = 2.0f;
        if (!(std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(center.z)))
            center = glm::vec3(0.0f);
        target      = center;
        frameRadius = 0.5f * diag;
        distance    = diag * 1.5f;  // == the pre-2.4 radius*3 framing magnitude
        yaw         = kInitYaw;
        pitch       = kInitPitch;
    }
};

}  // namespace rav

#endif  // _WIN32
