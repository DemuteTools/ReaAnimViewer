// SPDX-License-Identifier: MIT

#include "renderer.h"

#ifdef _WIN32

#include <cmath>
#include <cstddef>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "gl_loader.h"  // modern-GL pointers routed via the #define table

namespace rav {
namespace {

// #version 330 — the spike's proven profile; legacy wglCreateContext grants a
// high-enough compatibility profile on the reference workstation (Spike Finding).
const char* kVertexSrc = R"GLSL(
#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec2 a_uv;
uniform mat4 u_mvp;
uniform mat4 u_model;     // world position for the specular view vector
uniform mat3 u_normal;    // transpose(inverse(mat3(model))) — correct under non-uniform scale
out vec3 v_worldpos;
out vec3 v_normal;
out vec2 v_uv;
void main() {
    v_worldpos = vec3(u_model * vec4(a_pos, 1.0));
    v_normal   = u_normal * a_normal;
    v_uv       = a_uv;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)GLSL";

const char* kFragmentSrc = R"GLSL(
#version 330 core
in vec3 v_worldpos;
in vec3 v_normal;
in vec2 v_uv;
uniform vec3  u_baseColor;
uniform vec3  u_specularColor;
uniform float u_shininess;
uniform vec3  u_viewPos;
uniform sampler2D u_baseColorTex;
uniform int   u_hasTexture;     // 0 → flat factor only (textureless / failed-resolve)
out vec4 frag;
void main() {
    vec3 N = normalize(v_normal);
    vec3 L = normalize(vec3(0.4, 0.9, 0.5));        // single hardcoded directional light
    vec3 V = normalize(u_viewPos - v_worldpos);
    vec3 H = normalize(L + V);                        // Blinn half-vector
    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), u_shininess);
    // Texture modulates the flat factor; u_hasTexture==0 leaves Story-2.1 output
    // unchanged. Sampled color is treated as linear (no sRGB decode) for MVP.
    vec3 base = u_baseColor;
    if (u_hasTexture != 0) base *= texture(u_baseColorTex, v_uv).rgb;
    vec3 c = base * (0.15 + 0.85 * diff) + u_specularColor * spec;
    frag = vec4(c, 1.0);
}
)GLSL";

// Camera constants (D14): 50 deg vertical FOV. near/far are derived per-asset from
// the AABB radius in SetAsset (an asset may be millimetres or kilometres across).
constexpr float kFovYDegrees = 50.0f;

GLuint CompileShader(GLenum type, const char* src, std::string& err)
{
    const GLuint s = glCreateShader(type);
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

}  // namespace

bool Renderer::Init(std::string& out_error)
{
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexSrc, out_error);
    if (!vs) { out_error = "vertex shader: " + out_error; return false; }
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentSrc, out_error);
    if (!fs) { glDeleteShader(vs); out_error = "fragment shader: " + out_error; return false; }

    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024] = {};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        out_error = std::string("link: ") + log;
        glDeleteProgram(prog);
        return false;
    }
    program_ = GpuProgram(prog);

    u_mvp_            = glGetUniformLocation(program_.get(), "u_mvp");
    u_model_          = glGetUniformLocation(program_.get(), "u_model");
    u_normal_         = glGetUniformLocation(program_.get(), "u_normal");
    u_base_color_     = glGetUniformLocation(program_.get(), "u_baseColor");
    u_specular_color_ = glGetUniformLocation(program_.get(), "u_specularColor");
    u_shininess_      = glGetUniformLocation(program_.get(), "u_shininess");
    u_view_pos_       = glGetUniformLocation(program_.get(), "u_viewPos");
    u_base_color_tex_ = glGetUniformLocation(program_.get(), "u_baseColorTex");
    u_has_texture_    = glGetUniformLocation(program_.get(), "u_hasTexture");
    // Only u_mvp is genuinely required (no draw is possible without it). The rest may
    // legitimately come back -1 if a driver's GLSL optimizer eliminates a uniform it
    // proves dead — glUniform*(-1, ...) is a documented no-op, so a -1 here must NOT
    // be fatal or a valid driver could brick the viewer.
    if (u_mvp_ < 0) {
        out_error = "shader is missing the required u_mvp uniform";
        return false;
    }

    // One shared VAO; the per-mesh attribute pointers are re-specified each draw
    // (the bound VBO changes per mesh). The buffers themselves live on the Asset.
    glGenVertexArrays(1, vao_.addr());

    // Fixed pipeline state (never changes between frames) — set once here, not on
    // the hot path. Right-handed, CCW front-facing, back-face culled (D3/D12).
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // Default framing until an asset arrives (keeps an empty viewport sane).
    view_ = glm::lookAt(glm::vec3(3.5f, 3.0f, 4.5f),
                        glm::vec3(0.0f, 0.0f, 0.0f),
                        glm::vec3(0.0f, 1.0f, 0.0f));
    view_pos_ = glm::vec3(3.5f, 3.0f, 4.5f);
    return true;
}

void Renderer::SetAsset(Asset&& asset)
{
    asset_ = std::move(asset);

    // Auto-fit the camera to the asset bounding box (D14). This guarantees AC1/AC3
    // "visible and framed" for any scale or orientation: a tilted non-canonical
    // file simply frames tilted (AR13), recovered later by Reset Camera (Story 2.4).
    glm::vec3 center = 0.5f * (asset_.aabbMin + asset_.aabbMax);
    float radius = 0.5f * glm::length(asset_.aabbMax - asset_.aabbMin);
    // `!(radius > eps)` (not `radius < eps`) so a NaN — which compares false against
    // everything — also falls into the fallback instead of producing a NaN camera.
    if (!(radius > 1e-4f)) radius = 1.0f;   // degenerate/empty/NaN bounds — don't divide by ~0
    if (!(std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(center.z)))
        center = glm::vec3(0.0f);

    // Three-quarter view direction (matches the Epic 1 cube angle), pulled back so
    // the whole sphere fits inside the 50-deg FOV with headroom.
    const glm::vec3 dir = glm::normalize(glm::vec3(0.7f, 0.6f, 0.9f));
    const glm::vec3 eye = center + dir * radius * 3.0f;
    view_     = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
    view_pos_ = eye;

    // near/far scaled to the model so depth precision holds at any unit scale.
    near_plane_ = radius * 0.01f;
    far_plane_  = radius * 100.0f;

    cached_aspect_ = -1.0f;  // impossible aspect → force a projection rebuild next frame
}

void Renderer::RenderFrame(float /*time_seconds*/, int width, int height)
{
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;

    glViewport(0, 0, width, height);

    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Nothing loaded yet (no file picked, or a load failed): a live but empty panel.
    if (asset_.meshes.empty())
        return;

    // Rebuild the view-projection only when the aspect actually changes (a resize).
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    if (aspect != cached_aspect_) {
        const glm::mat4 proj = glm::perspective(glm::radians(kFovYDegrees), aspect,
                                                near_plane_, far_plane_);
        view_proj_     = proj * view_;
        cached_aspect_ = aspect;
    }

    glUseProgram(program_.get());
    glBindVertexArray(vao_.get());

    // modelRoot is identity in 2.1 (render as-authored, AR13); kept general so a
    // future per-asset transform flows through mvp + the normal matrix unchanged.
    const glm::mat4& model = asset_.modelRoot;
    const glm::mat3 normal_mat = glm::transpose(glm::inverse(glm::mat3(model)));
    const glm::mat4 mvp = view_proj_ * model;

    glUniformMatrix4fv(u_mvp_,    1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix4fv(u_model_,  1, GL_FALSE, glm::value_ptr(model));
    glUniformMatrix3fv(u_normal_, 1, GL_FALSE, glm::value_ptr(normal_mat));
    glUniform3fv(u_view_pos_, 1, glm::value_ptr(view_pos_));
    // The diffuse sampler always reads texture unit 0; bind the name to it once per
    // frame (a no-op if the driver optimized the sampler out → location -1).
    glUniform1i(u_base_color_tex_, 0);

    const GLsizei stride = sizeof(SceneVertex);
    for (const SceneMesh& mesh : asset_.meshes) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vb.get());
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ib.get());

        // Re-specify the attribute layout against this mesh's VBO. Only 0/1/2
        // (pos/normal/uv) are enabled — bone attribs 3/4 are left for Epic 3 so the
        // buffer layout never churns.
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(SceneVertex, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(SceneVertex, normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(SceneVertex, uv));

        const SceneMaterial& mat = asset_.materials[mesh.materialIdx];
        glUniform3fv(u_base_color_,     1, glm::value_ptr(mat.baseColorFactor));
        glUniform3fv(u_specular_color_, 1, glm::value_ptr(mat.specularColor));
        glUniform1f(u_shininess_, mat.shininess);

        // Bind this material's diffuse texture to unit 0. A 0 handle (textureless or
        // failed-resolve material) sets u_hasTexture=0 → the shader uses the flat
        // factor. Three cheap GL calls, no heap (D2 hot-path discipline).
        const GLuint tex = mat.baseColor.get();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(u_has_texture_, tex != 0 ? 1 : 0);

        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount),
                       GL_UNSIGNED_INT, nullptr);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

void Renderer::Shutdown()
{
    // Releasing the RAII members frees the GL objects while the context is current
    // (the caller guarantees that). Clearing the Asset releases its per-mesh
    // VBO/IBO handles the same way. Move-assign empties so a second Shutdown / the
    // destructor is a no-op.
    asset_   = Asset();
    program_ = GpuProgram();
    vao_     = GpuVertexArray();
}

}  // namespace rav

#endif  // _WIN32
