// SPDX-License-Identifier: MIT

#include "renderer.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>   // std::move — shadow-map handle hand-off
#include <vector>    // floor grid vertex scratch (cold path)

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
uniform vec3  u_lightColor;     // Story 6.5.1: key-light colour (set by 6.5.3 light tool later)
uniform vec3  u_lightDir;       // key-light direction (toward the light)
uniform float u_ambient;        // ambient/fill amount so unlit faces stay readable (AC2)
uniform sampler2D u_normalMap;  // tangent-space normal map (linear); unit 1
uniform int   u_hasNormalMap;   // 0 → use the geometric normal (asset has no map, AC4)
uniform float u_specStrength;   // live specular strength (light tool) — sheen that sculpts form
uniform float u_normalStrength; // live normal-map relief boost (light tool); 1 = as-authored
out vec4 frag;

// Specular strength is now a live uniform (u_specStrength) driven by the light tool —
// dielectric skin/cloth reads matte by default, but Antho can dial in more sheen to sculpt
// the form (the FR16 metal-vs-dielectric distinction is relative and survives any scale).

vec3 enc(vec3 c) { return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)); }  // linear → sRGB

void main() {
    vec3 N = normalize(v_normal);
    // Perturb the normal with the normal map via a per-FRAGMENT tangent frame derived
    // from screen-space derivatives (Christian Schüler's cotangent frame). This replaces
    // the per-vertex tangent attribute, which silently FAILS on mirrored UVs: a character
    // whose left/right halves share UV space (almost every game/Mixamo body) has tangents
    // that cancel to ~0 along the mirror seam, so a vertex-tangent TBN drops the normal
    // map exactly where a body straddles the seam (the torso) while limbs off the seam
    // look fine. The derivative frame is built from the actual rasterized position + UV,
    // so it is correct for ANY mesh — mirrored, un-tangented, or skinned — no vertex
    // tangent needed. (This is how engines do robust normal mapping.)
    if (u_hasNormalMap != 0) {
        vec3 dp1 = dFdx(v_worldpos);
        vec3 dp2 = dFdy(v_worldpos);
        vec2 duv1 = dFdx(v_uv);
        vec2 duv2 = dFdy(v_uv);
        vec3 dp2perp = cross(dp2, N);
        vec3 dp1perp = cross(N, dp1);
        vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
        vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
        // Guard the degenerate frame: a mesh with NO usable UVs (or a zero-UV-gradient
        // triangle) leaves v_uv constant → dFdx/dFdy(v_uv)=0 → T=B=0 → inversesqrt(0)=+Inf
        // → a NaN normal → black/garbage fragments. u_hasNormalMap gates on the MATERIAL
        // carrying a normal map, not on the drawn mesh having UVs, so this IS reachable.
        // When the frame is degenerate, keep the geometric normal — exactly AC4's "an asset
        // without UVs renders unchanged". (Cold path: the gate-tested assets all have UVs.)
        float maxlen2 = max(dot(T, T), dot(B, B));
        if (maxlen2 > 1e-12) {
            float invmax = inversesqrt(maxlen2);
            mat3 TBN = mat3(T * invmax, B * invmax, N);
            vec3 nt = texture(u_normalMap, v_uv).rgb * 2.0 - 1.0;   // [0,1] → [-1,1]
            nt.xy *= u_normalStrength;   // boost the tangent-space tilt → stronger relief
            N = normalize(TBN * nt);
        }
    }
    vec3 L = normalize(u_lightDir);
    vec3 V = normalize(u_viewPos - v_worldpos);
    vec3 H = normalize(L + V);                        // Blinn half-vector
    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), u_shininess);
    // Energy-normalize the Blinn-Phong lobe: a BROAD highlight (low shininess = a rough
    // or rough-metallic surface) must be DIM, not full-bright. Without this, a glTF metal
    // (metallic≈1, roughness≈1 → shininess floored at ~2) gets a huge bright white lobe
    // that WASHES the whole surface to pale grey and buries the texture detail. The
    // (n+8)/(8π) Blinn-Phong normalization makes broad lobes dim and tight lobes bright —
    // dielectrics (dim 0.04 specular) are visually unchanged; metals stop blowing out.
    spec *= (u_shininess + 8.0) / (8.0 * 3.14159265);
    // Base colour is sampled from a GL_SRGB8_ALPHA8 texture → the GPU already decoded it
    // to LINEAR on sample (Story 6.5.1 AC1), so all lighting below is linear-correct.
    vec3 base = u_baseColor;
    if (u_hasTexture != 0) base *= texture(u_baseColorTex, v_uv).rgb;

    // Balanced ambient/fill (AC2): a face turned from the key light keeps u_ambient of
    // its colour instead of crushing to near-black.
    vec3 c = base * (u_ambient + (1.0 - u_ambient) * diff) * u_lightColor;
    c += u_specularColor * spec * u_specStrength * u_lightColor;
    // Encode linear → sRGB on the final write (AC1). NOT GL_FRAMEBUFFER_SRGB: the default
    // framebuffer is a legacy non-sRGB pixel format, so the shader encode is the robust
    // path and must be the ONLY one (enabling both would double-encode / over-brighten).
    frag = vec4(enc(c), 1.0);
}
)GLSL";

// ---- Story 6.5.4 floor + shadow ---------------------------------------------
// The floor is a flat-colour ground plane + grid in its OWN minimal program (no
// lighting, no sRGB encode — display-space greys tuned at Antho's gate). It outputs
// colour directly, multiplied only by the PCF shadow factor sampled from the depth map.
const char* kFloorVertexSrc = R"GLSL(
#version 330 core
layout(location=0) in vec3 a_pos;
uniform mat4 u_mvp;
uniform mat4 u_model;
out vec3 v_worldpos;
void main() {
    v_worldpos  = vec3(u_model * vec4(a_pos, 1.0));   // world pos → sample the shadow map (Task 3)
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
)GLSL";

const char* kFloorFragmentSrc = R"GLSL(
#version 330 core
in vec3 v_worldpos;
uniform vec3  u_color;
uniform vec3  u_lightColor;     // key-light colour — tints the floor so coloured light reads
                                // across the whole scene (a red light reddens the ground too)
uniform mat4  u_lightSpace;     // world → light clip; rebuilt each frame from light_dir_
uniform sampler2D u_shadowMap;  // depth from the light POV (unit 0)
uniform float u_shadowOn;       // 0 → no shadow (quality Off)
uniform int   u_pcfRadius;      // 0 = 1 tap (Low), 1 = 3x3 (Mid), 2 = 5x5 (High)
uniform float u_shadowTexel;    // 1/shadow_map_size (square map)
out vec4 frag;

// kShadowFloor: the floor keeps this fraction of its colour where fully shadowed (it
// darkens, not to black, so the grid stays legible). Tunable at the gate.
const float kShadowFloor = 0.45;

void main() {
    float shadow = 1.0;   // 1 = fully lit, 0 = fully shadowed
    if (u_shadowOn > 0.5) {
        vec4 lp = u_lightSpace * vec4(v_worldpos, 1.0);
        vec3 proj = lp.xyz / lp.w * 0.5 + 0.5;   // → [0,1] depth-map space
        // Outside the light frustum → treat as lit (CLAMP_TO_EDGE + this bounds-check
        // avoid needing a border-colour enum).
        if (proj.x >= 0.0 && proj.x <= 1.0 && proj.y >= 0.0 && proj.y <= 1.0 && proj.z <= 1.0) {
            const float bias = 0.0015;   // constant depth bias to kill self-shadow acne
            float cur = proj.z - bias;
            float lit = 0.0, taps = 0.0;
            for (int x = -u_pcfRadius; x <= u_pcfRadius; ++x)
                for (int y = -u_pcfRadius; y <= u_pcfRadius; ++y) {
                    float d = texture(u_shadowMap, proj.xy + vec2(float(x), float(y)) * u_shadowTexel).r;
                    lit  += (cur <= d) ? 1.0 : 0.0;   // lit if nothing nearer to the light
                    taps += 1.0;
                }
            shadow = lit / taps;
        }
    }
    // Tint by the key-light colour (no other lighting — the floor stays flat/unlit) so the
    // 6.5.3 light-colour tool visibly affects the whole scene, not only the model.
    frag = vec4(u_color * mix(kShadowFloor, 1.0, shadow) * u_lightColor, 1.0);
}
)GLSL";

// Shadow-map size + PCF radius per quality level. Off = 0 (no pass). The PCF radius
// widens the softening kernel with quality (Low 1 tap → High 5x5).
int ShadowSizeFor(ShadowQuality q)
{
    switch (q) {
        case ShadowQuality::Low:  return 512;
        case ShadowQuality::Mid:  return 1024;
        case ShadowQuality::High: return 2048;
        case ShadowQuality::Off:  default: return 0;
    }
}
int PcfRadiusFor(ShadowQuality q)
{
    switch (q) {
        case ShadowQuality::Mid:  return 1;   // 3x3
        case ShadowQuality::High: return 2;   // 5x5
        case ShadowQuality::Low:  default: return 0;  // 1 tap
    }
}

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
    u_light_color_    = glGetUniformLocation(program_.get(), "u_lightColor");
    u_light_dir_      = glGetUniformLocation(program_.get(), "u_lightDir");
    u_ambient_        = glGetUniformLocation(program_.get(), "u_ambient");
    u_normal_map_     = glGetUniformLocation(program_.get(), "u_normalMap");
    u_has_normal_map_ = glGetUniformLocation(program_.get(), "u_hasNormalMap");
    u_spec_strength_   = glGetUniformLocation(program_.get(), "u_specStrength");
    u_normal_strength_ = glGetUniformLocation(program_.get(), "u_normalStrength");
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

    // Story 6.5.4 — build the always-on floor program + grid mesh (non-fatal: a floor-
    // shader failure leaves floor_program_ empty and the floor draw is skipped, the rest
    // of the viewport still runs, AR17). Then allocate the default-quality (Mid) shadow
    // map so the very first asset casts a shadow without the user touching the tool — a
    // cold path, the per-frame path stays allocation-free (D2). An allocation failure here
    // is non-fatal too: SetShadowQuality falls back to Off and the floor still draws.
    BuildFloor();
    SetShadowQuality(shadow_quality_);
    return true;
}

// Re-points the shared mesh VAO's attributes at `mesh`'s VBO/IBO. Identical layout to the
// visible draw loop (Story 3.3); factored so the shadow depth pass and the visible pass
// specify it the same way. boneIds is an INTEGER attribute (glVertexAttribIPointer, §D).
void Renderer::BindMeshAttribs(const SceneMesh& mesh)
{
    const GLsizei stride = sizeof(SceneVertex);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vb.get());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ib.get());
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SceneVertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SceneVertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SceneVertex, uv));
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 4, GL_INT, stride, (void*)offsetof(SceneVertex, boneIds));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SceneVertex, boneWeights));
}

void Renderer::BuildFloor()
{
    std::string err;
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, kFloorVertexSrc, err);
    if (!vs) { LogWarn("floor vertex shader failed (%s) - no ground plane this session", err.c_str()); return; }
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFloorFragmentSrc, err);
    if (!fs) { glDeleteShader(vs); LogWarn("floor fragment shader failed (%s) - no ground plane", err.c_str()); return; }

    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) { glDeleteProgram(prog); LogWarn("floor shader link failed - no ground plane"); return; }
    floor_program_ = GpuProgram(prog);

    u_floor_mvp_          = glGetUniformLocation(prog, "u_mvp");
    u_floor_model_        = glGetUniformLocation(prog, "u_model");
    u_floor_color_        = glGetUniformLocation(prog, "u_color");
    u_floor_light_color_  = glGetUniformLocation(prog, "u_lightColor");
    u_floor_light_space_  = glGetUniformLocation(prog, "u_lightSpace");
    u_floor_shadow_map_   = glGetUniformLocation(prog, "u_shadowMap");
    u_floor_shadow_on_    = glGetUniformLocation(prog, "u_shadowOn");
    u_floor_pcf_radius_   = glGetUniformLocation(prog, "u_pcfRadius");
    u_floor_shadow_texel_ = glGetUniformLocation(prog, "u_shadowTexel");

    // Build the floor geometry ONCE (cold path): a unit quad in XZ at y=0, [-0.5,0.5]²
    // (4 verts, TRIANGLE_STRIP), followed by an NxN grid of lines packed into the SAME
    // buffer. The model-scaled placement matrix (per frame) sizes it to the asset.
    constexpr int kDiv = 20;          // grid divisions
    std::vector<glm::vec3> verts;
    verts.reserve(4 + (kDiv + 1) * 4);
    verts.push_back(glm::vec3(-0.5f, 0.0f, -0.5f));   // quad (strip): the 4 corners
    verts.push_back(glm::vec3(-0.5f, 0.0f,  0.5f));
    verts.push_back(glm::vec3( 0.5f, 0.0f, -0.5f));
    verts.push_back(glm::vec3( 0.5f, 0.0f,  0.5f));
    for (int i = 0; i <= kDiv; ++i) {                 // grid lines (GL_LINES)
        const float t = -0.5f + static_cast<float>(i) / kDiv;
        verts.push_back(glm::vec3(-0.5f, 0.0f, t));   // line along X at z=t
        verts.push_back(glm::vec3( 0.5f, 0.0f, t));
        verts.push_back(glm::vec3(t, 0.0f, -0.5f));   // line along Z at x=t
        verts.push_back(glm::vec3(t, 0.0f,  0.5f));
    }
    gridVertCount_ = static_cast<GLsizei>(verts.size() - 4);

    glGenVertexArrays(1, floor_vao_.addr());
    glGenBuffers(1, floor_vb_.addr());
    glBindVertexArray(floor_vao_.get());
    glBindBuffer(GL_ARRAY_BUFFER, floor_vb_.get());
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(glm::vec3)),
                 verts.data(), GL_STATIC_DRAW);
    // The floor VBO never changes (unlike per-mesh VBOs), so the VAO can capture its one
    // attribute pointer once here — at draw time we just bind floor_vao_.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

bool Renderer::AllocShadowMap(int size)
{
    // Depth-only texture: GL_NEAREST + CLAMP_TO_EDGE, no colour. Built on locals so a
    // failure mid-way leaves the current members untouched (the caller decides the
    // fallback). Cold path only (SetShadowQuality / Init).
    GpuImage tex;
    glGenTextures(1, tex.addr());
    if (!tex.get()) return false;
    glBindTexture(GL_TEXTURE_2D, tex.get());
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    GpuFramebuffer fbo;
    glGenFramebuffers(1, fbo.addr());
    if (!fbo.get()) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo.get());
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex.get(), 0);
    glDrawBuffer(GL_NONE);   // depth-only — no colour attachment (GL 1.1, no loader row)
    glReadBuffer(GL_NONE);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) return false;

    shadow_depth_tex_ = std::move(tex);
    shadow_fbo_       = std::move(fbo);
    shadow_map_size_  = size;
    return true;
}

void Renderer::SetShadowQuality(ShadowQuality q)
{
    shadow_quality_ = q;
    const int size = ShadowSizeFor(q);
    if (size == 0) {
        // Off: free the map so it adds zero per-frame AND zero memory cost; the depth
        // pass is skipped entirely (the floor still draws). Context is current (this is
        // called from Init or the tool handler during the render tick).
        shadow_fbo_       = GpuFramebuffer();
        shadow_depth_tex_ = GpuImage();
        shadow_map_size_  = 0;
        return;
    }
    if (size == shadow_map_size_ && shadow_fbo_.get() && shadow_depth_tex_.get())
        return;  // already allocated at this size (re-select of the same level)

    if (!AllocShadowMap(size)) {
        // Non-fatal (AR17): drop to Off and keep running. Only happens on a quality change
        // (cold path), so this LogWarn is naturally bounded — not a per-frame storm.
        LogWarn("shadow map %dx%d could not be allocated - shadows off (the viewport still runs)",
                size, size);
        shadow_quality_   = ShadowQuality::Off;
        shadow_fbo_       = GpuFramebuffer();
        shadow_depth_tex_ = GpuImage();
        shadow_map_size_  = 0;
    }
}

void Renderer::DrawFloor()
{
    if (!floor_visible_) return;         // ground hidden by the tool (Antho post-gate toggle)
    if (!floor_program_.get()) return;   // floor shader failed to build → no ground plane

    // Placement (per frame, stack math): centre under the model, sit at its feet
    // (aabbMin.y), sized generously to the framing radius so it reads as ground.
    const glm::vec3 center = 0.5f * (asset_.aabbMin + asset_.aabbMax);
    const float extent = cam_.frameRadius * 6.0f;
    const glm::mat4 M =
        glm::translate(glm::mat4(1.0f), glm::vec3(center.x, asset_.aabbMin.y, center.z)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(extent, 1.0f, extent));
    const glm::mat4 mvp = view_proj_ * M;

    glUseProgram(floor_program_.get());
    glBindVertexArray(floor_vao_.get());
    glUniformMatrix4fv(u_floor_mvp_,   1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix4fv(u_floor_model_, 1, GL_FALSE, glm::value_ptr(M));
    glUniform3fv(u_floor_light_color_, 1, glm::value_ptr(light_color_));  // tint by the key light

    const bool shadows = (shadow_quality_ != ShadowQuality::Off && shadow_map_size_ > 0 &&
                          shadow_depth_tex_.get());
    glUniform1f(u_floor_shadow_on_, shadows ? 1.0f : 0.0f);
    if (shadows) {
        glUniformMatrix4fv(u_floor_light_space_, 1, GL_FALSE, glm::value_ptr(light_space_));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, shadow_depth_tex_.get());
        glUniform1i(u_floor_shadow_map_, 0);
        glUniform1i(u_floor_pcf_radius_, PcfRadiusFor(shadow_quality_));
        glUniform1f(u_floor_shadow_texel_, 1.0f / static_cast<float>(shadow_map_size_));
    }

    // Visible from below (no culling), and polygon-offset the solid plane so the grid
    // lines drawn at the same y don't z-fight it. Depth test/write stay on (opaque).
    glDisable(GL_CULL_FACE);

    const glm::vec3 kSolid(0.30f, 0.31f, 0.34f);
    glUniform3fv(u_floor_color_, 1, glm::value_ptr(kSolid));
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_POLYGON_OFFSET_FILL);

    const glm::vec3 kGrid(0.42f, 0.44f, 0.48f);
    glUniform3fv(u_floor_color_, 1, glm::value_ptr(kGrid));
    glDrawArrays(GL_LINES, 4, gridVertCount_);

    glEnable(GL_CULL_FACE);
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

    // Story 6.5.5 — MSAA toggle (FR52). One cheap GL-state call per frame (no allocation,
    // D2): turns the multisample resolve on/off for the visible passes. Inert if the context
    // owns no multisample buffer (wglChoosePixelFormatARB unavailable → legacy fallback), so
    // it is always safe to call (AR17). The shadow depth pass binds its own FBO and is
    // unaffected. Re-applied every frame so a hide/show or resize never leaves it stale.
    if (msaa_on_) glEnable(GL_MULTISAMPLE);
    else          glDisable(GL_MULTISAMPLE);

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

    // Shared mesh-program uniforms (constant across the depth + visible passes — only
    // u_mvp and u_skinned vary per pass/mesh below). u_mvp is set here for the visible
    // pass; the shadow depth pass overwrites it with the light-space mvp and the visible
    // loop restores it (Story 6.5.4 RenderFrame re-order).
    glUniformMatrix4fv(u_mvp_,    1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix4fv(u_model_,  1, GL_FALSE, glm::value_ptr(model));
    glUniformMatrix3fv(u_normal_, 1, GL_FALSE, glm::value_ptr(normal_mat));
    glUniform3fv(u_view_pos_, 1, glm::value_ptr(view_pos_));
    // The diffuse sampler always reads texture unit 0; bind the name to it once per
    // frame (a no-op if the driver optimized the sampler out → location -1).
    glUniform1i(u_base_color_tex_, 0);
    // Normal-map sampler reads texture unit 1 (bound per-material in the draw loop).
    glUniform1i(u_normal_map_, 1);
    // Lighting defaults (Story 6.5.1) — set once per frame from members so Story 6.5.3's
    // light tool can later drive them. Handful of glUniform* calls, no heap (D2).
    glUniform3fv(u_light_color_, 1, glm::value_ptr(light_color_));
    glUniform3fv(u_light_dir_,   1, glm::value_ptr(light_dir_));
    glUniform1f(u_ambient_, ambient_);
    glUniform1f(u_spec_strength_,   spec_strength_);    // live light-tool knobs (6.5.x polish)
    glUniform1f(u_normal_strength_, normal_strength_);

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

    // ---- Shadow depth pass (Story 6.5.4) ------------------------------------
    // Render the model's depth from the light's POV into the offscreen depth FBO, so the
    // floor can test occlusion. Skipped entirely when quality==Off (zero added cost). The
    // pose/palette computed above is reused (skinned models cast a posed shadow); only
    // u_mvp changes to the light-space transform. State that this pass changes — the bound
    // FBO, the viewport, and the cull face — is fully restored before the visible passes.
    // The floor is the only shadow receiver, so a hidden floor needs no depth pass at all.
    const bool do_shadow = (floor_visible_ &&
                            shadow_quality_ != ShadowQuality::Off && shadow_map_size_ > 0 &&
                            shadow_fbo_.get() && shadow_depth_tex_.get());
    if (do_shadow) {
        // Light-space ortho fitted to the model, rebuilt every frame from light_dir_ so the
        // 6.5.3 light pad sweeps the shadow (AC3). light_dir_ points TOWARD the light.
        const glm::vec3 center = 0.5f * (asset_.aabbMin + asset_.aabbMax);
        const float fr = cam_.frameRadius;
        const glm::vec3 lightPos = center + light_dir_ * (fr * 2.0f);
        const glm::vec3 up = (std::abs(light_dir_.y) > 0.99f) ? glm::vec3(0, 0, 1)
                                                              : glm::vec3(0, 1, 0);
        const glm::mat4 lightView = glm::lookAt(lightPos, center, up);
        const float r = fr * 1.2f;
        const glm::mat4 lightProj = glm::ortho(-r, r, -r, r, fr * 0.05f, fr * 5.0f);
        light_space_ = lightProj * lightView;

        glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_.get());
        glViewport(0, 0, shadow_map_size_, shadow_map_size_);
        glClear(GL_DEPTH_BUFFER_BIT);
        glCullFace(GL_FRONT);  // cull front faces during the depth pass to reduce acne
        glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, glm::value_ptr(light_space_ * model));
        for (const SceneMesh& mesh : asset_.meshes) {
            BindMeshAttribs(mesh);
            glUniform1i(u_skinned_, (pose_valid && mesh.skinned) ? 1 : 0);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount),
                           GL_UNSIGNED_INT, nullptr);
        }
        glCullFace(GL_BACK);                       // restore the fixed cull state
        glBindFramebuffer(GL_FRAMEBUFFER, 0);      // visible output goes back to FB0
        glViewport(0, 0, width, height);           // restore the window viewport
    }

    // ---- Floor pass (always) ------------------------------------------------
    // The always-on ground plane + grid, receiving the cast shadow via PCF. Switches to
    // floor_program_/floor_vao_; the visible mesh pass re-binds the mesh program after.
    DrawFloor();

    // ---- Visible mesh pass --------------------------------------------------
    // Re-bind the mesh program + VAO (DrawFloor switched both) and restore the visible
    // u_mvp (the depth pass overwrote it). The other shared uniforms + the palette persist
    // on the program object across the floor pass, so they need no re-upload.
    glUseProgram(program_.get());
    glBindVertexArray(vao_.get());
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, glm::value_ptr(mvp));

    for (const SceneMesh& mesh : asset_.meshes) {
        // Re-specify the attribute layout against this mesh's VBO (pos/normal/uv + bone
        // ids/weights). The shared VAO holds no per-mesh state, so each mesh re-points its
        // attributes at its own buffer — the same setup the depth pass used (BindMeshAttribs).
        BindMeshAttribs(mesh);
        // The skinned/static choice rides on u_skinned alone: when 0, the shader's skinning
        // branch is dead and a static mesh is bit-identical to Epic 2 (AC3). Skin only when
        // the pose computed cleanly this frame (clip-less / no-op → static, §E / AC7).
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

        // Bind this material's normal map to unit 1; a 0 handle (no map) sets
        // u_hasNormalMap=0 → the shader keeps the geometric normal (AC4). Same flat-
        // fallback discipline as the diffuse above; three cheap GL calls, no heap (D2).
        const GLuint nmap = mat.normalMap.get();
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, nmap);
        // Story 6.5.5 — AND-gate the per-material flag with the global normal-maps toggle
        // (FR52). Off → flag 0 → the shader falls back to the geometric normal exactly as it
        // already does for assets that carry no map (6.5.1 AC4); no GLSL change, no new uniform.
        glUniform1i(u_has_normal_map_, (normal_maps_on_ && nmap != 0) ? 1 : 0);

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

    // Story 6.5.4 — release the floor program/buffers and the shadow FBO + depth texture
    // (RAII frees them while the context is current; NFR-R3 symmetric teardown). Reset the
    // sizes so a fresh StartRendering→Init rebuilds cleanly.
    floor_program_    = GpuProgram();
    floor_vao_        = GpuVertexArray();
    floor_vb_         = GpuBuffer();
    gridVertCount_    = 0;
    shadow_fbo_       = GpuFramebuffer();
    shadow_depth_tex_ = GpuImage();
    shadow_map_size_  = 0;
}

}  // namespace rav

#endif  // _WIN32
