// SPDX-License-Identifier: MIT

#include "renderer.h"

#ifdef _WIN32

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "console_log.h"
#include "gl_loader.h"  // modern-GL pointers routed via the #define table

namespace rav {
namespace {

struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec3 color;
};

// #version 330 — the spike's proven profile; legacy wglCreateContext grants a
// high-enough compatibility profile on the reference workstation (Spike Finding).
const char* kVertexSrc = R"GLSL(
#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec3 a_color;
uniform mat4 u_mvp;
uniform mat3 u_normal;   // transpose(inverse(mat3(model))) — correct under non-uniform scale too
out vec3 v_normal;
out vec3 v_color;
void main() {
    v_normal = u_normal * a_normal;        // world-space normal
    v_color  = a_color;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)GLSL";

const char* kFragmentSrc = R"GLSL(
#version 330 core
in vec3 v_normal;
in vec3 v_color;
out vec4 frag;
void main() {
    vec3 N = normalize(v_normal);
    vec3 L = normalize(vec3(0.4, 0.9, 0.5));   // light from above-ish so "up" reads bright
    float d = max(dot(N, L), 0.0);
    vec3 c = v_color * (0.25 + 0.75 * d);      // ambient + lambert
    frag = vec4(c, 1.0);
}
)GLSL";

// Camera constants (D14): 50 deg vertical FOV, near 0.01, far 1000.
constexpr float kFovYDegrees = 50.0f;
constexpr float kNearPlane   = 0.01f;
constexpr float kFarPlane    = 1000.0f;

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

// Appends one axis-aligned cube face (4 verts + 6 indices) to the buffers.
void AppendFace(std::vector<Vertex>& verts, std::vector<uint32_t>& idx,
                glm::vec3 center, glm::vec3 normal, glm::vec3 u, glm::vec3 v,
                glm::vec3 color)
{
    const uint32_t base = static_cast<uint32_t>(verts.size());
    verts.push_back({center - u - v, normal, color});
    verts.push_back({center + u - v, normal, color});
    verts.push_back({center + u + v, normal, color});
    verts.push_back({center - u + v, normal, color});
    // CCW winding when viewed from outside (front-facing per D12, glFrontFace CCW).
    idx.insert(idx.end(), {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});
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

    u_mvp_    = glGetUniformLocation(program_.get(), "u_mvp");
    u_normal_ = glGetUniformLocation(program_.get(), "u_normal");
    if (u_mvp_ < 0 || u_normal_ < 0) {
        out_error = "shader is missing an expected uniform (u_mvp / u_normal)";
        return false;
    }

    // --- Build the test scene geometry (allocation here is fine; Init only) ---
    std::vector<Vertex>   verts;
    std::vector<uint32_t> idx;

    // Ground: a quad in the XZ plane at y=0, normal +Y. Drawn first; its indices
    // occupy the front of the shared buffer. A cool grey so the lit cube stands out.
    const glm::vec3 kGround(0.22f, 0.24f, 0.28f);
    const float g = 6.0f;
    AppendFace(verts, idx, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0),
               glm::vec3(g, 0, 0), glm::vec3(0, 0, g), kGround);
    const size_t ground_count = idx.size();

    // Cube: centred at (0, 1, 0), half-extent 0.6. Distinct per-face colours so
    // the spin is obvious; the +Y (top) face is bright green to make "up" plain.
    const glm::vec3 c(0, 1, 0);
    const float h = 0.6f;
    AppendFace(verts, idx, c + glm::vec3(0, 0, h), glm::vec3(0, 0, 1),
               glm::vec3(h, 0, 0), glm::vec3(0, h, 0), glm::vec3(0.20f, 0.45f, 0.95f));  // +Z blue
    AppendFace(verts, idx, c + glm::vec3(0, 0, -h), glm::vec3(0, 0, -1),
               glm::vec3(-h, 0, 0), glm::vec3(0, h, 0), glm::vec3(0.95f, 0.30f, 0.55f)); // -Z pink
    AppendFace(verts, idx, c + glm::vec3(h, 0, 0), glm::vec3(1, 0, 0),
               glm::vec3(0, 0, -h), glm::vec3(0, h, 0), glm::vec3(0.95f, 0.45f, 0.20f)); // +X orange
    AppendFace(verts, idx, c + glm::vec3(-h, 0, 0), glm::vec3(-1, 0, 0),
               glm::vec3(0, 0, h), glm::vec3(0, h, 0), glm::vec3(0.25f, 0.80f, 0.80f));  // -X teal
    AppendFace(verts, idx, c + glm::vec3(0, h, 0), glm::vec3(0, 1, 0),
               glm::vec3(h, 0, 0), glm::vec3(0, 0, -h), glm::vec3(0.35f, 0.90f, 0.35f)); // +Y green (up)
    AppendFace(verts, idx, c + glm::vec3(0, -h, 0), glm::vec3(0, -1, 0),
               glm::vec3(h, 0, 0), glm::vec3(0, 0, h), glm::vec3(0.85f, 0.80f, 0.25f));  // -Y yellow
    const size_t cube_count = idx.size() - ground_count;

    // Precompute the per-mesh draw ranges into the shared IBO once (byte offsets
    // never change), so RenderFrame draws without re-deriving the layout.
    ground_draw_ = {static_cast<GLsizei>(ground_count), nullptr};
    cube_draw_   = {static_cast<GLsizei>(cube_count),
                    reinterpret_cast<const void*>(ground_count * sizeof(uint32_t))};

    glGenVertexArrays(1, vao_.addr());
    glBindVertexArray(vao_.get());

    glGenBuffers(1, vbo_.addr());
    glBindBuffer(GL_ARRAY_BUFFER, vbo_.get());
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(Vertex)),
                 verts.data(), GL_STATIC_DRAW);

    glGenBuffers(1, ibo_.addr());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_.get());
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(idx.size() * sizeof(uint32_t)),
                 idx.data(), GL_STATIC_DRAW);

    const GLsizei stride = sizeof(Vertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, color));

    glBindVertexArray(0);

    // Fixed pipeline state (never changes between frames) — set once here, not on
    // the hot path. Right-handed, CCW front-facing, back-face culled (D3/D12).
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // Camera is fixed (three-quarter view, Y-up); the view matrix is constant.
    // Projection depends only on aspect, so it's rebuilt lazily in RenderFrame.
    view_ = glm::lookAt(glm::vec3(3.5f, 3.0f, 4.5f),
                        glm::vec3(0.0f, 0.8f, 0.0f),
                        glm::vec3(0.0f, 1.0f, 0.0f));
    return true;
}

void Renderer::RenderFrame(float time_seconds, int width, int height)
{
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;

    glViewport(0, 0, width, height);

    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Rebuild the view-projection only when the aspect actually changes (a window
    // resize), not every frame — it's otherwise constant (AC3 still satisfied).
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    if (aspect != cached_aspect_) {
        const glm::mat4 proj = glm::perspective(glm::radians(kFovYDegrees), aspect,
                                                kNearPlane, kFarPlane);
        view_proj_     = proj * view_;
        cached_aspect_ = aspect;
    }

    glUseProgram(program_.get());
    glBindVertexArray(vao_.get());

    auto draw = [&](const glm::mat4& model, const DrawRange& range) {
        const glm::mat3 normal_mat = glm::transpose(glm::inverse(glm::mat3(model)));
        glUniformMatrix3fv(u_normal_, 1, GL_FALSE, glm::value_ptr(normal_mat));
        const glm::mat4 mvp = view_proj_ * model;
        glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, glm::value_ptr(mvp));
        glDrawElements(GL_TRIANGLES, range.count, GL_UNSIGNED_INT, range.offset);
    };

    // Ground: static (identity model).
    draw(glm::mat4(1.0f), ground_draw_);

    // Cube: spins about Y so the motion is obvious while "up" stays up.
    draw(glm::rotate(glm::mat4(1.0f), time_seconds * 0.9f, glm::vec3(0, 1, 0)),
         cube_draw_);

    glBindVertexArray(0);
}

void Renderer::Shutdown()
{
    // Releasing the RAII members frees the GL objects while the context is
    // current (the caller guarantees that). Move-assign empty handles so a
    // second Shutdown / the destructor is a no-op.
    program_ = GpuProgram();
    vao_     = GpuVertexArray();
    vbo_     = GpuBuffer();
    ibo_     = GpuBuffer();
}

}  // namespace rav

#endif  // _WIN32
