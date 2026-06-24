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

    // Takes ownership of a loaded model and computes the auto-fit framing camera
    // from its AABB (center/radius → eye distance, near/far). Requires a current
    // GL context (it replaces the previously-held Asset, freeing its GL handles).
    void SetAsset(Asset&& asset);

    // Renders one frame into the currently-bound default framebuffer. Sets the
    // viewport from width/height and recomputes projection on aspect change so a
    // resize keeps the correct aspect ratio. No allocation on this path (D2 hot-path).
    void RenderFrame(float time_seconds, int width, int height);

    // Releases GL resources (including the held Asset's buffers). Must run while
    // the GL context is current. Safe to call more than once.
    void Shutdown();

private:
    GpuProgram     program_;
    GpuVertexArray vao_;
    Asset          asset_;

    // Framing camera, recomputed in SetAsset from the asset AABB. view_proj_ is
    // rebuilt only when the aspect ratio changes (a resize), not every frame.
    glm::mat4 view_{1.0f};
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
};

}  // namespace rav

#endif  // _WIN32
