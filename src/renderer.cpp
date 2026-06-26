// SPDX-License-Identifier: MIT

#include "renderer.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "animation.h"    // ComputePose — the per-frame D13 palette (header-only, glm-pure)
#include "console_log.h"  // LogWarn for the >128-bone palette cap (AC6/§F)
#include "gl_loader.h"    // modern-GL pointers routed via the #define table

namespace rav {
namespace {

// #version 330 — the spike's proven profile; legacy wglCreateContext grants a
// high-enough compatibility profile on the reference workstation (Spike Finding).
const char* kVertexSrc = R"GLSL(
#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec2 a_uv;
layout(location=3) in ivec4 a_boneIds;     // integer attribute — fed via glVertexAttribIPointer
layout(location=4) in vec4 a_boneWeights;
uniform mat4 u_mvp;
uniform mat4 u_model;       // world position for the specular view vector
uniform mat3 u_normal;      // transpose(inverse(mat3(model))) — correct under non-uniform scale
uniform mat4 u_bones[128];  // D13 skinning palette (globalMat * inverseBind) per skeleton
uniform int  u_skinned;     // 0 → static path is bit-identical to Epic 2 (AC3)
out vec3 v_worldpos;
out vec3 v_normal;
out vec2 v_uv;
void main() {
    vec3 pos = a_pos;
    vec3 nrm = a_normal;
    if (u_skinned != 0) {
        // Linear blend skinning (D13): weight-blend the <=4 influencing palette
        // matrices, then transform in mesh-local space BEFORE u_model/u_mvp (the
        // skinned verts are stored un-baked there, §C).
        // Clamp to [0,127] before indexing: a >128-bone rig (capped at upload, §F)
        // can carry a global bone id >= 128, and dynamic indexing past u_bones[128]
        // is undefined behaviour in GLSL. Clamping pins the overflow to the cap so it
        // degrades to best-effort instead of reading garbage/crashing (AC6). For the
        // clean <=128-bone fixtures every id is already in range — this is a no-op.
        ivec4 ids = clamp(a_boneIds, ivec4(0), ivec4(127));
        mat4 skin = a_boneWeights.x * u_bones[ids.x]
                  + a_boneWeights.y * u_bones[ids.y]
                  + a_boneWeights.z * u_bones[ids.z]
                  + a_boneWeights.w * u_bones[ids.w];
        // aiProcess_LimitBoneWeights trims to 4 WITHOUT renormalizing, so a trimmed
        // vertex sums to <1 (visible shrinkage) and an unrigged vertex sums to 0
        // (collapse to origin). Renormalize when positive; else fall back to identity.
        float wsum = dot(a_boneWeights, vec4(1.0));
        if (wsum > 0.0) skin /= wsum; else skin = mat4(1.0);
        pos = vec3(skin * vec4(a_pos, 1.0));
        nrm = mat3(skin) * a_normal;     // rigid/uniform-scale bones → mat3(skin) is exact enough (§F)
    }
    v_worldpos = vec3(u_model * vec4(pos, 1.0));
    v_normal   = u_normal * nrm;
    v_uv       = a_uv;
    gl_Position = u_mvp * vec4(pos, 1.0);
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
    u_bones_          = glGetUniformLocation(program_.get(), "u_bones");
    u_skinned_        = glGetUniformLocation(program_.get(), "u_skinned");
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

    // Auto-fit-on-load IS Reset (D14): the first frame a user sees is exactly what
    // the Reset View button reproduces. A tilted non-canonical file simply frames
    // tilted (AR13), recovered by Reset (Story 2.4). cam_.Reset carries the same
    // degenerate/NaN-bounds guards the inline framing used to.
    ResetCamera();

    // Size the skinning palette + scratch ONCE here (cold load path) so the per-frame
    // ComputePose never allocates (D2). ComputePose only runs when out_palette.size()
    // == bones.size(), so size to the FULL bone count; the upload count alone is capped
    // at 128 in RenderFrame (§E). A rig over the std140 mat4[128] cap warns once here —
    // not per frame (AC6/§F). Empty skeleton → empty buffers → the static Epic 2 path.
    const size_t bone_count = asset_.skeleton.bones.size();
    palette_.assign(bone_count, glm::mat4(1.0f));
    pose_scratch_.assign(bone_count, glm::mat4(1.0f));
    if (bone_count > 128)
        LogWarn("skeleton has %zu bones, exceeding the 128-bone palette cap - only the "
                "first 128 are uploaded; vertices bound to bones beyond 128 are clamped "
                "to the cap and may render incorrectly (robust remap is post-MVP)", bone_count);
}

void Renderer::ResetCamera()
{
    cam_.Reset(asset_.aabbMin, asset_.aabbMax);

    // near/far scaled to the model so depth precision holds at any unit scale (kept
    // from 2.1 — strictly better than D14's fixed 0.01/1000 for mm/km-authored files).
    // frameRadius == 0.5 * diagonal, already guarded against degenerate/NaN bounds.
    const float radius = cam_.frameRadius;
    near_plane_ = radius * 0.01f;
    far_plane_  = radius * 100.0f;

    cached_aspect_ = -1.0f;  // impossible aspect → force a projection rebuild next frame
}

void Renderer::RenderFrame(float anim_time_seconds, bool loop, int width, int height)
{
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;

    glViewport(0, 0, width, height);

    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Nothing loaded yet (no file picked, or a load failed): a live but empty panel.
    if (asset_.meshes.empty())
        return;

    // The projection depends only on aspect — rebuild it just on a resize.
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    if (aspect != cached_aspect_) {
        proj_          = glm::perspective(glm::radians(kFovYDegrees), aspect,
                                          near_plane_, far_plane_);
        cached_aspect_ = aspect;
    }

    // The view changes every frame (a drag moves the camera), so derive it from cam_
    // here, not in SetAsset. Two stack mat4 ops + Eye() — no heap, no GL state churn
    // (D2 hot-path / the 1.2 zero-alloc-in-RenderFrame rule still holds).
    view_      = cam_.ViewMatrix();
    view_pos_  = cam_.Eye();
    view_proj_ = proj_ * view_;

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

    // Per-frame D13 pose: compute the skinning palette once (shared by every skinned
    // mesh of this skeleton) and upload it before the draw loop. The caller selects
    // how a time outside the clip is handled (Story 4.3): the transport path passes
    // loop==false so t CLAMPS to [0, duration] and the rig HOLDS the last frame at the
    // item's end — and an item resized LONGER than the clip (Story 4.4) still holds,
    // not wraps (AC3); the no-item fixture fallback passes loop==true to keep the
    // Epic-3 free-running fmod loop. ComputePose writes into pre-sized buffers (no
    // alloc, D2); it no-ops on a size mismatch, leaving palette_ stale, so pose_valid
    // gates whether any mesh is allowed to draw skinned this frame (§E / AC7).
    bool pose_valid = false;
    if (!asset_.skeleton.bones.empty() && !asset_.animations.empty()) {
        const SceneAnimation& clip = asset_.animations[0];
        float t = anim_time_seconds;
        if (clip.duration > 0.0f) {
            t = loop ? std::fmod(anim_time_seconds, clip.duration)
                     : std::min(std::max(anim_time_seconds, 0.0f), clip.duration);
        } else {
            t = 0.0f;
        }
        ComputePose(asset_.skeleton, clip, t, palette_, pose_scratch_);
        // pose_valid must mirror ComputePose's FULL precondition, not just the palette
        // size SetAsset already guaranteed: a clip whose channels don't match the bone
        // count makes ComputePose no-op (leaving the identity-filled palette), and skinning
        // with that identity palette would place the un-baked verts wrong. Re-check the
        // channel size too so a malformed clip falls back to the static bind-pose path
        // (§E / AC7) instead of skinning silently. (3.2-deferred no-op-detection item.)
        const size_t nb_bones = asset_.skeleton.bones.size();
        pose_valid = (!palette_.empty() && palette_.size() == nb_bones &&
                      clip.channels.size() == nb_bones);
        if (pose_valid) {
            const GLsizei nb = std::min<GLsizei>(static_cast<GLsizei>(palette_.size()), 128);
            glUniformMatrix4fv(u_bones_, nb, GL_FALSE, glm::value_ptr(palette_[0]));
        }
    }

    const GLsizei stride = sizeof(SceneVertex);
    for (const SceneMesh& mesh : asset_.meshes) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vb.get());
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ib.get());

        // Re-specify the attribute layout against this mesh's VBO (pos/normal/uv +
        // bone ids/weights below). The shared VAO holds no per-mesh state, so each
        // mesh re-points its attributes at its own buffer, exactly as 2.1 did for 0/1/2.
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(SceneVertex, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(SceneVertex, normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(SceneVertex, uv));

        // Bone attributes 3/4 are specified for EVERY mesh against its own VBO (every
        // SceneVertex carries boneIds/boneWeights — zero-filled on a static mesh). The
        // skinned/static choice rides on u_skinned alone: when 0, the shader's skinning
        // branch is dead and a_boneIds/a_boneWeights are never read, so a static mesh is
        // bit-identical to Epic 2 (AC3) — and the GL loader needs exactly ONE new row
        // (glVertexAttribIPointer), not also a disable row (AC8). boneIds is an INTEGER
        // attribute: the I-variant feeds the ids verbatim; the float glVertexAttribPointer
        // would convert/normalize them and skin by the wrong bones (§D). Skin only when
        // the pose computed cleanly this frame (clip-less / no-op → static, §E / AC7).
        glEnableVertexAttribArray(3);
        glVertexAttribIPointer(3, 4, GL_INT, stride,
                               (void*)offsetof(SceneVertex, boneIds));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride,
                              (void*)offsetof(SceneVertex, boneWeights));
        glUniform1i(u_skinned_, (pose_valid && mesh.skinned) ? 1 : 0);

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
