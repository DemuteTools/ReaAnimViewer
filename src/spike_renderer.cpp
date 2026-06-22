// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.

#include "spike_renderer.h"

#include <cstddef>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "spike_gl.h"
#include "reaper_api.h"  // ShowConsoleMsg

namespace spike {
namespace {

const char* kVertexSrc = R"GLSL(
#version 330 core
layout(location=0) in vec3  a_pos;
layout(location=1) in vec3  a_normal;
layout(location=2) in vec2  a_uv;
layout(location=3) in ivec4 a_boneIds;
layout(location=4) in vec4  a_weights;
uniform mat4 u_mvp;
uniform mat4 u_model;
uniform mat4 u_palette[64];
uniform int  u_skinned;
out vec3 v_normal;
void main() {
    vec4 pos;
    vec3 nrm;
    if (u_skinned == 1) {
        mat4 skin = a_weights.x * u_palette[a_boneIds.x]
                  + a_weights.y * u_palette[a_boneIds.y]
                  + a_weights.z * u_palette[a_boneIds.z]
                  + a_weights.w * u_palette[a_boneIds.w];
        pos = skin * vec4(a_pos, 1.0);
        nrm = mat3(skin) * a_normal;
    } else {
        pos = vec4(a_pos, 1.0);
        nrm = a_normal;
    }
    v_normal = mat3(u_model) * nrm;
    gl_Position = u_mvp * pos;
}
)GLSL";

const char* kFragmentSrc = R"GLSL(
#version 330 core
in vec3 v_normal;
out vec4 frag;
void main() {
    vec3 N = normalize(v_normal);
    vec3 L = normalize(vec3(0.4, 0.8, 0.6));
    float d = max(dot(N, L), 0.0);
    vec3 c = vec3(0.15) + vec3(0.85) * d;   // ambient + lambert (spike: no textures)
    frag = vec4(c, 1.0);
}
)GLSL";

unsigned CompileShader(GLenum type, const char* src, std::string& err)
{
    const unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        err = log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// --- animation sampling ---

template <typename T>
T SampleVec(const std::vector<std::pair<float, T>>& keys, float t, const T& fallback)
{
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || t <= keys.front().first) return keys.front().second;
    if (t >= keys.back().first) return keys.back().second;
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (t < keys[i + 1].first) {
            const float t0 = keys[i].first, t1 = keys[i + 1].first;
            const float a = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
            return glm::mix(keys[i].second, keys[i + 1].second, a);
        }
    }
    return keys.back().second;
}

glm::quat SampleQuat(const std::vector<std::pair<float, glm::quat>>& keys, float t)
{
    if (keys.empty()) return glm::quat(1, 0, 0, 0);
    if (keys.size() == 1 || t <= keys.front().first) return keys.front().second;
    if (t >= keys.back().first) return keys.back().second;
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (t < keys[i + 1].first) {
            const float t0 = keys[i].first, t1 = keys[i + 1].first;
            const float a = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
            return glm::slerp(keys[i].second, keys[i + 1].second, a);
        }
    }
    return keys.back().second;
}

}  // namespace

bool Renderer::Init(std::string& outError)
{
    const unsigned vs = CompileShader(GL_VERTEX_SHADER, kVertexSrc, outError);
    if (!vs) { outError = "vertex shader: " + outError; return false; }
    const unsigned fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentSrc, outError);
    if (!fs) { outError = "fragment shader: " + outError; return false; }

    m_prog = glCreateProgram();
    glAttachShader(m_prog, vs);
    glAttachShader(m_prog, fs);
    glLinkProgram(m_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(m_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetProgramInfoLog(m_prog, sizeof(log), nullptr, log);
        outError = std::string("link: ") + log;
        return false;
    }

    m_uMvp     = glGetUniformLocation(m_prog, "u_mvp");
    m_uModel   = glGetUniformLocation(m_prog, "u_model");
    m_uPalette = glGetUniformLocation(m_prog, "u_palette");
    m_uSkinned = glGetUniformLocation(m_prog, "u_skinned");
    return true;
}

void Renderer::SetModel(const Model& model)
{
    m_model = model;
    m_hasModel = !model.vertices.empty();
    m_indexCount = static_cast<int>(model.indices.size());
    if (!m_hasModel) return;

    if (!m_vao) glGenVertexArrays(1, &m_vao);
    if (!m_vbo) glGenBuffers(1, &m_vbo);
    if (!m_ibo) glGenBuffers(1, &m_ibo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(model.vertices.size() * sizeof(Vertex)),
                 model.vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(model.indices.size() * sizeof(uint32_t)),
                 model.indices.data(), GL_STATIC_DRAW);

    const GLsizei stride = sizeof(Vertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 4, GL_INT, stride, (void*)offsetof(Vertex, boneIds));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, boneWeights));

    glBindVertexArray(0);
}

void Renderer::DestroyFbo()
{
    if (m_fbo)      { glDeleteFramebuffers(1, &m_fbo);    m_fbo = 0; }
    if (m_colorTex) { glDeleteTextures(1, &m_colorTex);   m_colorTex = 0; }
    if (m_depthRbo) { glDeleteRenderbuffers(1, &m_depthRbo); m_depthRbo = 0; }
}

void Renderer::Resize(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == m_w && h == m_h && m_fbo) return;
    m_w = w; m_h = h;

    DestroyFbo();

    glGenTextures(1, &m_colorTex);
    glBindTexture(GL_TEXTURE_2D, m_colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenRenderbuffers(1, &m_depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthRbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        ShowConsoleMsg("[FBXAV-spike] FBO incomplete\n");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    m_readBytes.assign(static_cast<size_t>(w) * h * 4, 0);
    m_packed.assign(static_cast<size_t>(w) * h, 0xFF101012u);  // dark grey, opaque
}

const uint32_t* Renderer::RenderToPixels(float t, int& outW, int& outH)
{
    outW = m_w; outH = m_h;
    if (!m_fbo) return nullptr;

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_w, m_h);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (m_hasModel) {
        // Camera auto-fit to AABB, slow orbit so deformation reads in 3D.
        const glm::vec3 center = 0.5f * (m_model.aabbMin + m_model.aabbMax);
        const float radius = glm::max(0.001f, 0.5f * glm::length(m_model.aabbMax - m_model.aabbMin));
        const float dist = radius * 3.0f;
        const float yaw = t * 0.6f;
        const glm::vec3 eye = center + glm::vec3(std::sin(yaw), 0.35f, std::cos(yaw)) * dist;
        const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0, 1, 0));
        const float aspect = static_cast<float>(m_w) / static_cast<float>(m_h);
        const glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect,
                                                glm::max(0.001f, radius * 0.05f), radius * 20.0f);
        const glm::mat4 mvp = proj * view * m_model.modelRoot;

        glUseProgram(m_prog);
        glUniformMatrix4fv(m_uMvp, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniformMatrix4fv(m_uModel, 1, GL_FALSE, glm::value_ptr(m_model.modelRoot));

        const bool skinned = m_model.skinned();
        glUniform1i(m_uSkinned, skinned ? 1 : 0);

        if (skinned) {
            const float dur = (m_model.clip.duration > 0.0f) ? m_model.clip.duration : 1.0f;
            const float tt = std::fmod(t, dur);

            const size_t n = m_model.bones.size();
            std::vector<glm::mat4> local(n), global(n), palette(n);
            for (size_t i = 0; i < n; ++i) local[i] = m_model.bones[i].localBind;

            // Override locals that have animation channels with the sampled pose.
            for (const AnimChannel& ch : m_model.clip.channels) {
                if (ch.boneIdx < 0 || static_cast<size_t>(ch.boneIdx) >= n) continue;
                const glm::vec3 tr = SampleVec(ch.translation, tt, glm::vec3(0.0f));
                const glm::quat ro = SampleQuat(ch.rotation, tt);
                const glm::vec3 sc = SampleVec(ch.scale, tt, glm::vec3(1.0f));
                local[ch.boneIdx] = glm::translate(glm::mat4(1.0f), tr) *
                                    glm::mat4_cast(ro) *
                                    glm::scale(glm::mat4(1.0f), sc);
            }

            for (size_t i = 0; i < n; ++i) {
                const int p = m_model.bones[i].parentIdx;
                global[i] = (p < 0) ? local[i] : global[p] * local[i];
                palette[i] = global[i] * m_model.bones[i].inverseBind;
            }

            const int count = static_cast<int>(glm::min<size_t>(n, kMaxBones));
            glUniformMatrix4fv(m_uPalette, count, GL_FALSE, glm::value_ptr(palette[0]));
        }

        glBindVertexArray(m_vao);
        glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }

    // Readback: GPU -> CPU (the bridge cost the spike measures). GL framebuffers
    // are bottom-up; we flip rows here and pack to 0xRRGGBBAA so ReaImGui's
    // Image_SetPixels_Array gets an upright, correctly-formatted image.
    glReadPixels(0, 0, m_w, m_h, GL_RGBA, GL_UNSIGNED_BYTE, m_readBytes.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    for (int y = 0; y < m_h; ++y) {
        const uint8_t* src = &m_readBytes[static_cast<size_t>(m_h - 1 - y) * m_w * 4];
        uint32_t* dst = &m_packed[static_cast<size_t>(y) * m_w];
        for (int x = 0; x < m_w; ++x) {
            const uint8_t r = src[x * 4 + 0], g = src[x * 4 + 1],
                          b = src[x * 4 + 2], a = src[x * 4 + 3];
            dst[x] = (static_cast<uint32_t>(r) << 24) | (static_cast<uint32_t>(g) << 16) |
                     (static_cast<uint32_t>(b) << 8) | static_cast<uint32_t>(a);
        }
    }
    return m_packed.data();
}

void Renderer::Shutdown()
{
    DestroyFbo();
    if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
    if (m_ibo) { glDeleteBuffers(1, &m_ibo); m_ibo = 0; }
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_prog) { glDeleteProgram(m_prog); m_prog = 0; }
}

}  // namespace spike
