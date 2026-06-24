// SPDX-License-Identifier: MIT
//
// Direct-to-window scene renderer (raw GL). Draws into the currently-bound
// default framebuffer (framebuffer 0) and never binds an offscreen FBO — the
// Spike-0 viewport architecture (Spec Change Log 2026-06-23). Story 2.1 swapped
// the Epic 1 built-in test cube for a loaded `Asset`: Init compiles the Blinn-Phong
// material shader, SetAsset stores a parsed model and frames the camera to its
// bounding box, and RenderFrame draws the asset's meshes. With no asset set the
// viewport just clears (a live but empty panel — never a crash).

#pragma once

#include <string>

#ifdef _WIN32

#include <glm/glm.hpp>

#include "camera.h"
#include "gpu_resources.h"
#include "scene.h"

namespace rav {

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Compiles the Blinn-Phong shader and creates the shared VAO. Requires a
    // current GL context. Returns false and fills out_error on failure.
    bool Init(std::string& out_error);

    // Takes ownership of a loaded model and auto-fits the camera to its AABB
    // (cam_.Reset → center/distance, plus near/far). Requires a current GL context
    // (it replaces the previously-held Asset, freeing its GL handles).
    void SetAsset(Asset&& asset);

    // Renders one frame into the currently-bound default framebuffer. Sets the
    // viewport from width/height and recomputes projection on aspect change so a
    // resize keeps the correct aspect ratio. Rebuilds the view from cam_ EVERY frame
    // (a drag moves the camera). No allocation on this path (D2 hot-path).
    void RenderFrame(float time_seconds, int width, int height);

    // Mutable access to the orbit camera so the window proc can drive it from mouse
    // input (g_renderer.Camera().Orbit(...) / .Zoom(...) / .Pan(...)).
    OrbitCamera& Camera() { return cam_; }

    // Re-frames the camera on the current asset's AABB and recomputes near/far — the
    // one-call entry point for the Reset View button (FR25 / Journey 2 recovery).
    void ResetCamera();

    // Releases GL resources (including the held Asset's buffers). Must run while
    // the GL context is current. Safe to call more than once.
    void Shutdown();

private:
    GpuProgram     program_;
    GpuVertexArray vao_;
    Asset          asset_;
    OrbitCamera    cam_;         // D14 orbit state; the view is derived from it each frame

    // view_/view_pos_/view_proj_ are DERIVED from cam_ each frame in RenderFrame (a
    // drag moves the camera). Only the projection is cached across frames and rebuilt
    // on an aspect change (a resize) — it does not depend on the camera.
    glm::mat4 view_{1.0f};
    glm::mat4 proj_{1.0f};
    glm::mat4 view_proj_{1.0f};
    glm::vec3 view_pos_{0.0f};   // camera world position → u_viewPos (specular)
    float     near_plane_  = 0.01f;
    float     far_plane_   = 1000.0f;
    float     cached_aspect_ = -1.0f;  // impossible aspect → first frame always rebuilds

    int u_mvp_           = -1;
    int u_model_         = -1;
    int u_normal_        = -1;
    int u_base_color_    = -1;
    int u_specular_color_ = -1;
    int u_shininess_     = -1;
    int u_view_pos_      = -1;
    int u_base_color_tex_ = -1;  // sampler2D bound to texture unit 0
    int u_has_texture_   = -1;   // 0 → flat factor (textureless / failed-resolve)
};

}  // namespace rav

#endif  // _WIN32
