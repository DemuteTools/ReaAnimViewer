// SPDX-License-Identifier: MIT
//
// Direct-to-window scene renderer (raw GL). Draws into the currently-bound
// default framebuffer (framebuffer 0) and never binds an offscreen FBO — the
// Spike-0 viewport architecture (Spec Change Log 2026-06-23). For Story 1.2 the
// scene is a built-in test scene (a lit rotating cube above a ground reference)
// that proves the shader/VAO pipeline, depth test, back-face cull, and a visually
// unambiguous Y-up orientation. Epic 2 swaps the test scene for a loaded Asset
// behind this same interface.

#pragma once

#include <string>

#ifdef _WIN32

#include <glm/glm.hpp>

#include "gpu_resources.h"

namespace rav {

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Compiles the shader and builds the test-scene buffers. Requires a current
    // GL context. Returns false and fills out_error on failure.
    bool Init(std::string& out_error);

    // Renders one frame into the currently-bound default framebuffer. Sets the
    // viewport from width/height and recomputes projection each call so a resize
    // keeps the correct aspect ratio. No allocation on this path (D2 hot-path).
    void RenderFrame(float time_seconds, int width, int height);

    // Releases GL resources. Must run while the GL context is current. Safe to
    // call more than once; the destructor also releases via RAII.
    void Shutdown();

private:
    // A contiguous slice of the shared index buffer (byte offset precomputed in
    // Init, so the hot path issues a draw without re-deriving the layout). Epic 2
    // grows this from two fixed meshes to a per-asset list of ranges.
    struct DrawRange {
        GLsizei     count  = 0;
        const void* offset = nullptr;
    };

    GpuProgram     program_;
    GpuVertexArray vao_;
    GpuBuffer      vbo_;
    GpuBuffer      ibo_;

    DrawRange ground_draw_;
    DrawRange cube_draw_;

    // view_ is constant (fixed camera); view_proj_ is rebuilt only when the
    // aspect ratio changes, not every frame.
    glm::mat4 view_{1.0f};
    glm::mat4 view_proj_{1.0f};
    float     cached_aspect_ = 0.0f;

    int u_mvp_    = -1;
    int u_normal_ = -1;
};

}  // namespace rav

#endif  // _WIN32
