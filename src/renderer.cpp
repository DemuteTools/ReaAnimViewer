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
uniform sampler2D u_specularMap;// Story 6.5.7: artist specular map (linear); unit 2
uniform int   u_hasSpecularMap; // 0 → uniform dielectric sheen (no map, AC3)
uniform sampler2D u_glossMap;   // Story 6.5.7: artist glossiness map (linear); unit 3
uniform int   u_hasGlossMap;    // 0 → use the scalar u_shininess (no map, AC3)
uniform float u_specStrength;   // live specular strength (light tool) — sheen that sculpts form
uniform float u_normalStrength; // live normal-map relief boost (light tool); 1 = as-authored
uniform float u_modelAlpha;     // spec 10-3c: 1 = opaque (Model mode); Skeleton mode fades the model
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
    // Story 6.5.7 — per-pixel specular exponent from the artist's glossiness map. Where the
    // material carries the map, the Blinn exponent is driven PER-PIXEL (matte zones → low
    // exponent → broad dim lobe; oily/shiny zones → high exponent → tight bright highlight)
    // instead of the single scalar u_shininess. The [1, 200] range was tuned at Antho's gate
    // (matte skin → 1, oily highlights → 200). No map → flag 0 → the scalar path (AC3,
    // byte-for-byte as before).
    float shin = u_shininess;
    if (u_hasGlossMap != 0) shin = mix(1.0, 200.0, texture(u_glossMap, v_uv).r);
    float spec = pow(max(dot(N, H), 0.0), shin);
    // Energy-normalize the Blinn-Phong lobe: a BROAD highlight (low shininess = a rough
    // or rough-metallic surface) must be DIM, not full-bright. Without this, a glTF metal
    // (metallic≈1, roughness≈1 → shininess floored at ~2) gets a huge bright white lobe
    // that WASHES the whole surface to pale grey and buries the texture detail. The
    // (n+8)/(8π) Blinn-Phong normalization makes broad lobes dim and tight lobes bright —
    // dielectrics (dim 0.04 specular) are visually unchanged; metals stop blowing out.
    spec *= (shin + 8.0) / (8.0 * 3.14159265);
    // Base colour is sampled from a GL_SRGB8_ALPHA8 texture → the GPU already decoded it
    // to LINEAR on sample (Story 6.5.1 AC1), so all lighting below is linear-correct.
    vec3 base = u_baseColor;
    if (u_hasTexture != 0) base *= texture(u_baseColorTex, v_uv).rgb;

    // Balanced ambient/fill (AC2): a face turned from the key light keeps u_ambient of
    // its colour instead of crushing to near-black.
    vec3 c = base * (u_ambient + (1.0 - u_ambient) * diff) * u_lightColor;
    // Story 6.5.7 — per-pixel specular intensity from the artist's specular map. specTint
    // modulates WHERE/how strong the sheen falls: matte zones (dark map) get no sheen, oily
    // zones (bright map) get full sheen → a multi-material head/body seam reads continuous
    // (the trigger). The dielectric base u_specularColor STAYS (AC2 — the flat authored
    // COLOR_SPECULAR is still ignored, the 6.5.1 "too plastic" fix); the map only modulates
    // it. No map → specTint = 1 → identical to the pre-6.5.7 uniform sheen (AC3).
    vec3 specTint = vec3(1.0);
    if (u_hasSpecularMap != 0) specTint = texture(u_specularMap, v_uv).rgb;
    c += u_specularColor * spec * u_specStrength * specTint * u_lightColor;
    // Encode linear → sRGB on the final write (AC1). NOT GL_FRAMEBUFFER_SRGB: the default
    // framebuffer is a legacy non-sRGB pixel format, so the shader encode is the robust
    // path and must be the ONLY one (enabling both would double-encode / over-brighten).
    // Spec 10-3c: u_modelAlpha is 1 in Model mode (blending off: the same pixels as before).
    frag = vec4(enc(c), u_modelAlpha);
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
    u_specular_map_     = glGetUniformLocation(program_.get(), "u_specularMap");
    u_has_specular_map_ = glGetUniformLocation(program_.get(), "u_hasSpecularMap");
    u_gloss_map_        = glGetUniformLocation(program_.get(), "u_glossMap");
    u_has_gloss_map_    = glGetUniformLocation(program_.get(), "u_hasGlossMap");
    u_spec_strength_   = glGetUniformLocation(program_.get(), "u_specStrength");
    u_normal_strength_ = glGetUniformLocation(program_.get(), "u_normalStrength");
    u_model_alpha_     = glGetUniformLocation(program_.get(), "u_modelAlpha");
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

    // The floor geometry (quad + grid lines, one buffer) depends on the asset and the grid
    // step, so it is (re)filled by RebuildFloorGeometry. Re-specifying the buffer's data
    // keeps the VAO's attribute pointer valid, so the VAO captures it once here and the
    // draw just binds floor_vao_.
    glGenVertexArrays(1, floor_vao_.addr());
    glGenBuffers(1, floor_vb_.addr());
    glBindVertexArray(floor_vao_.get());
    glBindBuffer(GL_ARRAY_BUFFER, floor_vb_.get());
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    RebuildFloorGeometry();
}

void Renderer::SetGridStep(float meters)
{
    if (!(meters > 0.0f) || meters == grid_step_m_) return;
    grid_step_m_ = meters;
    RebuildFloorGeometry();
}

void Renderer::RebuildFloorGeometry()
{
    if (!floor_vb_.get()) return;  // floor program failed to build → nothing is drawn

    // The grid step in scene units: a 1 m cell is 100 units in an FBX (centimetres) and
    // 1 unit in a glTF (metres).
    const float mpu  = (asset_.metersPerUnit > 0.0f) ? asset_.metersPerUnit : 1.0f;
    const float step = grid_step_m_ / mpu;

    // Same footprint as before the metric grid (6x the framing radius, centred under the
    // model), rounded OUT to whole cells on world multiples of the step so a line always
    // runs through the origin and the cells read as absolute metres. A model smaller than
    // a cell still sits on at least one whole cell. Capped so a 1 m grid under a kilometre-wide
    // scene stays a bounded buffer (the floor then shrinks around the centre).
    constexpr int kMaxCells = 2000;
    const glm::vec3 c = 0.5f * (asset_.aabbMin + asset_.aabbMax);
    const float half = 3.0f * cam_.frameRadius;
    auto span = [&](float centre, float& lo, int& cells) {
        float l = std::floor((centre - half) / step);
        float h = std::ceil((centre + half) / step);
        if (!(std::isfinite(l) && std::isfinite(h)) || h <= l) {  // degenerate → one cell
            l = std::floor(centre / step); h = l + 1.0f;
        }
        if (h - l > static_cast<float>(kMaxCells)) {
            l = std::floor(centre / step) - static_cast<float>(kMaxCells / 2);
            h = l + static_cast<float>(kMaxCells);
        }
        lo = l * step;
        cells = static_cast<int>(h - l);
    };
    float x0 = 0.0f, z0 = 0.0f;
    int nx = 1, nz = 1;
    span(c.x, x0, nx);
    span(c.z, z0, nz);
    const float x1 = x0 + static_cast<float>(nx) * step;
    const float z1 = z0 + static_cast<float>(nz) * step;

    std::vector<glm::vec3> verts;
    verts.reserve(4 + static_cast<size_t>(nx + 1 + nz + 1) * 2);
    verts.push_back(glm::vec3(x0, 0.0f, z0));   // quad (strip): the 4 corners
    verts.push_back(glm::vec3(x0, 0.0f, z1));
    verts.push_back(glm::vec3(x1, 0.0f, z0));
    verts.push_back(glm::vec3(x1, 0.0f, z1));
    for (int i = 0; i <= nz; ++i) {             // grid lines (GL_LINES) along X
        const float z = z0 + static_cast<float>(i) * step;
        verts.push_back(glm::vec3(x0, 0.0f, z));
        verts.push_back(glm::vec3(x1, 0.0f, z));
    }
    for (int i = 0; i <= nx; ++i) {             // ... and along Z
        const float x = x0 + static_cast<float>(i) * step;
        verts.push_back(glm::vec3(x, 0.0f, z0));
        verts.push_back(glm::vec3(x, 0.0f, z1));
    }
    gridVertCount_ = static_cast<GLsizei>(verts.size() - 4);

    glBindBuffer(GL_ARRAY_BUFFER, floor_vb_.get());
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(glm::vec3)),
                 verts.data(), GL_STATIC_DRAW);
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

bool Renderer::AllocMsaaTargets(int w, int h, int samples)
{
    // Story 6.5.6 — the offscreen multisample COLOUR + DEPTH renderbuffers + their FBO. Built on
    // locals so a failure mid-way leaves the current members untouched (the caller decides the
    // fallback — Off). Cold path only (the RenderFrame reconcile, on a level/size change). Mirrors
    // AllocShadowMap: gen into RAII handles via .addr(), attach, glCheckFramebufferStatus, RESTORE
    // the default binds before returning, move-assign into the members on success, false on any
    // GL failure so the caller falls back to Off (AR17). The caller has already clamped `samples`
    // to GL_MAX_SAMPLES (an unclamped count makes glRenderbufferStorageMultisample raise
    // GL_INVALID_VALUE).
    GpuRenderbuffer color;
    glGenRenderbuffers(1, color.addr());
    if (!color.get()) return false;
    glBindRenderbuffer(GL_RENDERBUFFER, color.get());
    // GL_RGBA8 (NOT sRGB): the mesh shader already encodes linear→sRGB on its final write (6.5.1)
    // and the window is plain RGBA8, so an RGBA8→RGBA8 resolve is byte-for-byte with no colour
    // shift. An sRGB renderbuffer here would double-encode and wash the image out.
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);

    GpuRenderbuffer depth;
    glGenRenderbuffers(1, depth.addr());
    if (!depth.get()) return false;
    glBindRenderbuffer(GL_RENDERBUFFER, depth.get());
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h);

    GpuFramebuffer fbo;
    glGenFramebuffers(1, fbo.addr());
    if (!fbo.get()) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo.get());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color.get());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_RENDERBUFFER, depth.get());
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) return false;

    msaa_color_rb_      = std::move(color);
    msaa_depth_rb_      = std::move(depth);
    msaa_fbo_           = std::move(fbo);
    msaa_alloc_w_       = w;
    msaa_alloc_h_       = h;
    msaa_alloc_samples_ = samples;
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

    // Placement (per frame, stack math): the geometry is already laid out in world XZ by
    // RebuildFloorGeometry (centred under the model, metric grid), so only lift it to the
    // model's feet (aabbMin.y).
    const glm::mat4 M =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, asset_.aabbMin.y, 0.0f));
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
    // The floor footprint follows the framing radius and the grid follows the asset's unit
    // (Epic 9), so rebuild it now that both are known.
    RebuildFloorGeometry();

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

DisplaySettings Renderer::CurrentDisplaySettings() const
{
    DisplaySettings s;
    s.light_color     = light_color_;
    s.light_dir       = light_dir_;
    s.ambient         = ambient_;
    s.spec_strength   = spec_strength_;
    s.normal_strength = normal_strength_;
    s.floor_visible   = floor_visible_;
    s.grid_step_m     = grid_step_m_;
    s.shadow_quality  = static_cast<int>(shadow_quality_);
    s.normal_maps     = normal_maps_on_;
    s.msaa_samples    = msaa_samples_;
    s.background      = glm::vec3(background_);
    return s;
}

void Renderer::ApplyDisplaySettings(const DisplaySettings& s, int max_msaa_samples)
{
    SetLightColor(s.light_color);
    SetLightDir(s.light_dir);
    SetAmbient(s.ambient);
    SetSpecStrength(s.spec_strength);
    SetNormalStrength(s.normal_strength);
    SetFloorVisible(s.floor_visible);
    SetGridStep(s.grid_step_m);   // rebuilds the grid only on a change
    // Only on a CHANGE of the requested level: SetShadowQuality may fall back to Off on an
    // allocation failure, which must not turn into one failing allocation per frame.
    const int shadow = std::clamp(s.shadow_quality, 0, 3);
    if (shadow != applied_shadow_request_) {
        applied_shadow_request_ = shadow;
        SetShadowQuality(static_cast<ShadowQuality>(shadow));
    }
    SetNormalMapsEnabled(s.normal_maps);
    int msaa = s.msaa_samples > 0 ? s.msaa_samples : 0;
    if (max_msaa_samples < 2) msaa = 0;
    else if (msaa > max_msaa_samples) msaa = max_msaa_samples;
    SetMsaaSamples(msaa);
    background_ = glm::vec4(s.background, 1.0f);
}

Asset Renderer::TakeAsset()
{
    Asset out = std::move(asset_);
    asset_ = Asset();          // a moved-from vector is valid but unspecified: make it empty
    palette_.clear();
    pose_scratch_.clear();
    return out;
}

void Renderer::RenderFrame(float anim_time_seconds, bool loop, int width, int height,
                           unsigned target_fbo)
{
    const GLuint target = static_cast<GLuint>(target_fbo);
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;
    // Spec 10-3c: what this frame draws, for the skeleton overlay (posed below, or nothing).
    last_w_ = width;
    last_h_ = height;
    last_pose_valid_ = false;

    // ---- Story 6.5.6 MSAA reconcile (cold path) -----------------------------
    // Decide this frame's SCENE target. With a level selected (msaa_samples_ > 0) the whole
    // scene renders into the offscreen multisample colour FBO and is blit-resolved to the
    // window at the end; at Off it renders straight to the window (FB0). The MS targets are
    // (re)allocated ONLY when the level or the window size changed (a resize changes
    // width/height) — the per-frame path is otherwise a few int compares, allocation-free
    // (D2). A failed allocation falls back to Off for this change without re-attempting the
    // failing alloc every frame (AR17). Mirrors the 6.5.4 AllocShadowMap cold-path discipline.
    const int want = msaa_samples_;
    if (want > 0) {
        if (msaa_alloc_w_ != width || msaa_alloc_h_ != height || msaa_alloc_samples_ != want) {
            if (!AllocMsaaTargets(width, height, want)) {
                // Non-fatal: free any partial targets and RECORD the requested config so we
                // treat it as Off without retrying the (failing) alloc next frame — a later
                // level change or resize changes the cache and gives it a fresh attempt.
                msaa_fbo_      = GpuFramebuffer();
                msaa_color_rb_ = GpuRenderbuffer();
                msaa_depth_rb_ = GpuRenderbuffer();
                msaa_alloc_w_ = width; msaa_alloc_h_ = height; msaa_alloc_samples_ = want;
                LogWarn("MSAA %dx targets could not be allocated - MSAA off (the viewport still runs)",
                        want);
            }
        }
    } else if (msaa_fbo_.get() || msaa_alloc_samples_ != 0) {
        // Off: free the targets so there is ZERO offscreen cost (AC4).
        msaa_fbo_      = GpuFramebuffer();
        msaa_color_rb_ = GpuRenderbuffer();
        msaa_depth_rb_ = GpuRenderbuffer();
        msaa_alloc_w_ = 0; msaa_alloc_h_ = 0; msaa_alloc_samples_ = 0;
    }
    const bool   msaa_on   = (want > 0 && msaa_fbo_.get() != 0);
    const GLuint scene_fbo = msaa_on ? msaa_fbo_.get() : target;

    // Everything (clear, shadow restore, floor, mesh) targets scene_fbo; the resolve at the
    // end of RenderFrame blits it to the target (the window for the viewer). At Off
    // scene_fbo == target → straight to it.
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
    glViewport(0, 0, width, height);

    glClearColor(background_.r, background_.g, background_.b, background_.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Nothing loaded yet (no file picked, or a load failed): a live but empty panel. Still
    // resolve the cleared MS buffer to the target so the empty panel isn't left stale/garbage.
    if (asset_.meshes.empty()) {
        if (msaa_on) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo_.get());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
            glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, target);
        }
        return;
    }

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
    // Story 6.5.7 — specular/glossiness samplers read units 2/3 (bound per-material below).
    glUniform1i(u_specular_map_, 2);
    glUniform1i(u_gloss_map_, 3);
    // Lighting defaults (Story 6.5.1) — set once per frame from members so Story 6.5.3's
    // light tool can later drive them. Handful of glUniform* calls, no heap (D2).
    glUniform3fv(u_light_color_, 1, glm::value_ptr(light_color_));
    glUniform3fv(u_light_dir_,   1, glm::value_ptr(light_dir_));
    glUniform1f(u_ambient_, ambient_);
    glUniform1f(u_spec_strength_,   spec_strength_);    // live light-tool knobs (6.5.x polish)
    glUniform1f(u_normal_strength_, normal_strength_);
    glUniform1f(u_model_alpha_, model_alpha_);           // spec 10-3c (1 = Model mode)

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
        last_pose_valid_ = pose_valid && pose_scratch_.size() == nb_bones;  // spec 10-3c
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
        glCullFace(GL_BACK);                          // restore the fixed cull state
        glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo); // back to the ACTIVE scene target (Story
                                                      // 6.5.6: the MS FBO when MSAA is on, else
                                                      // FB0 — NOT unconditionally 0, or the floor
                                                      // + mesh would draw to the window and the
                                                      // resolve would then overwrite it blank)
        glViewport(0, 0, width, height);              // restore the window viewport
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

    // Spec 10-3c -- Skeleton mode (model alpha < 1): a depth-only prepass of the meshes, then
    // the colour pass blended at that alpha, keeping only the nearest surface (GL_LEQUAL, depth
    // writes off), so the faded model never shows its own inside. Model mode skips all of it
    // (alpha 1, no blending: the pixels are the same as before). State restored after the loop.
    const bool faded = model_alpha_ < 1.0f;
    if (faded) {
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        for (const SceneMesh& mesh : asset_.meshes) {
            BindMeshAttribs(mesh);
            glUniform1i(u_skinned_, (pose_valid && mesh.skinned) ? 1 : 0);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount),
                           GL_UNSIGNED_INT, nullptr);
        }
        // Destination alpha untouched by the blend (a target's alpha stays as the floor left it).
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
    }

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

        // Story 6.5.7 — bind this material's specular + glossiness maps to units 2 and 3 via
        // glActiveTexture(GL_TEXTURE0 + n): GL_TEXTURE0..n are guaranteed contiguous, so no
        // GL_TEXTURE2/GL_TEXTURE3 enum is needed → gl_loader.h stays untouched. A 0 handle
        // (glTF metallic-roughness, or any material with no such map) sets the flag to 0 → the
        // shader keeps the uniform dielectric sheen / scalar exponent (AC3). Same flat-fallback
        // three-call rhythm as the diffuse/normal binds above; no heap (D2 hot-path).
        const GLuint smap = mat.specularMap.get();
        glActiveTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, smap);
        glUniform1i(u_has_specular_map_, smap != 0 ? 1 : 0);

        const GLuint gmap = mat.glossMap.get();
        glActiveTexture(GL_TEXTURE0 + 3);
        glBindTexture(GL_TEXTURE_2D, gmap);
        glUniform1i(u_has_gloss_map_, gmap != 0 ? 1 : 0);

        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount),
                       GL_UNSIGNED_INT, nullptr);
    }

    if (faded) {  // spec 10-3c: back to the fixed pipeline state (Init)
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glDisable(GL_BLEND);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // ---- Story 6.5.6 MSAA resolve -------------------------------------------
    // Blit-resolve the multisample colour into the target (FB0 for the viewer). src/dst are the
    // same size, so GL_NEAREST does a straight multisample resolve. Leaves the target bound on
    // return so ImGui (DrawToolUi, drawn after RenderFrame) and SwapBuffers land on the window
    // over the resolved scene. At Off scene_fbo == target → no blit, the target was bound
    // throughout (identical to pre-6.5.6 for the viewer). Story 11-2: target, not FB0.
    if (msaa_on) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_fbo_.get());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
    }
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
    applied_shadow_request_ = -1;  // Story 11-2: re-apply the requested level after a re-Init

    // Story 6.5.6 — release the offscreen MSAA targets (RAII frees them while the context is
    // current; NFR-R3 symmetric teardown). Reset the alloc cache so a fresh StartRendering
    // reallocates cleanly on the next level/size.
    msaa_fbo_           = GpuFramebuffer();
    msaa_color_rb_      = GpuRenderbuffer();
    msaa_depth_rb_      = GpuRenderbuffer();
    msaa_alloc_w_       = 0;
    msaa_alloc_h_       = 0;
    msaa_alloc_samples_ = 0;
}

}  // namespace rav

#endif  // _WIN32
