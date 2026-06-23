// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.
//
// Raw-GL offscreen renderer: GPU vertex skinning into an FBO, then a CPU
// readback (the only bridge ReaImGui supports — see SPIKE0_FINDINGS.md). The
// packed RGBA buffer it returns is handed to ReaImGui via Image_SetPixels_Array.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "spike_scene.h"

namespace spike {

class Renderer {
public:
    bool Init(std::string& outError);   // requires a current GL context
    void SetModel(const Model& model);  // uploads geometry to GPU
    void Resize(int w, int h);          // (re)creates the FBO color+depth

    // Draws the posed model into whatever framebuffer is currently bound (caller
    // sets viewport target). Used by both the FBO path (ReaImGui) and the direct
    // docked-GL-window path. Assumes a current GL context.
    void DrawScene(float t, int w, int h);

    // Poses the model at time t (seconds, looped) and returns a top-down packed
    // RGBA buffer (0xRRGGBBAA per pixel) of size outW*outH. Returns nullptr if
    // no FBO/model is ready.
    const uint32_t* RenderToPixels(float t, int& outW, int& outH);

    void Shutdown();

private:
    void DestroyFbo();

    Model m_model;
    bool  m_hasModel = false;

    unsigned m_prog = 0;
    unsigned m_vao = 0, m_vbo = 0, m_ibo = 0;
    int      m_indexCount = 0;

    unsigned m_fbo = 0, m_colorTex = 0, m_depthRbo = 0;
    int      m_w = 0, m_h = 0;

    std::vector<uint8_t>  m_readBytes;   // raw glReadPixels RGBA bytes
    std::vector<uint32_t> m_packed;      // flipped, packed 0xRRGGBBAA

    int m_uMvp = -1, m_uModel = -1, m_uPalette = -1, m_uSkinned = -1;
};

}  // namespace spike
