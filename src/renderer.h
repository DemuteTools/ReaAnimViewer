// SPDX-License-Identifier: MIT
//
// Direct-to-window scene renderer (raw GL). The VISIBLE output always goes to the
// currently-bound default framebuffer (framebuffer 0) — the Spike-0 viewport
// architecture (Spec Change Log 2026-06-23). Story 2.1 swapped the Epic 1 built-in
// test cube for a loaded `Asset`: Init compiles the Blinn-Phong material shader,
// SetAsset stores a parsed model and frames the camera to its bounding box, and
// RenderFrame draws the asset's meshes. With no asset set the viewport just clears
// (a live but empty panel — never a crash).
//
// Story 6.5.4 adds an always-on ground plane + grid and a real-time cast shadow. The
// shadow needs a depth render from the light's POV, so RenderFrame now binds a
// TRANSIENT offscreen DEPTH FBO for that one pass — a sanctioned DEVIATION from the
// original "never binds an offscreen FBO" rule (Antho-directed, AR20 Spec Change Log
// 2026-06-28): the FBO is bound ONLY during the depth pass and framebuffer 0 is always
// restored before the visible floor/mesh passes, so the visible surface is still FB0.
// Shadow quality (Off/Low/Mid/High) is a session-only knob driven by the 6.5.3 tool menu.
//
// Story 6.5.6 adds selectable MSAA (Off/2×/4×/8×). When a level is on, the WHOLE scene is
// rendered into an offscreen MULTISAMPLE COLOUR FBO and blit-resolved to the window — a
// SIBLING deviation to the 6.5.4 depth FBO (the first offscreen COLOUR pass; Antho-directed,
// AR20 2026-06-29). RenderFrame binds that FBO as the "scene target", the shadow pass restores
// to it (not unconditionally to FB0), and the resolve blits to FB0 before returning so ImGui
// and SwapBuffers land on the window. At Off the scene target IS FB0 (bit-identical to before,
// no offscreen cost). The MS targets are reallocated only on a level/resize change (cold path).

#pragma once

#include <string>
#include <vector>

#ifdef _WIN32

#include <glm/glm.hpp>

#include "camera.h"
#include "gpu_resources.h"
#include "scene.h"

namespace rav {

// Story 6.5.4 shadow-quality lever (a weak PC can dial it down/off). Maps to a
// shadow-map size + PCF tap radius: Off = no depth pass (zero added cost, the floor
// still draws), Low = 512 / 1 tap (hard), Mid = 1024 / 3x3, High = 2048 / 5x5.
enum class ShadowQuality { Off, Low, Mid, High };

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
    //
    // anim_time_seconds is the time to pose the rig at (seconds). loop selects how a
    // time outside the clip is handled (Story 4.3): loop==false (transport-driven)
    // CLAMPS to [0, duration] so the rig HOLDS the first/last frame at the item's
    // ends (AC3); loop==true keeps the Epic-3 free-running fmod loop used by the
    // no-item fixture fallback.
    void RenderFrame(float anim_time_seconds, bool loop, int width, int height);

    // Mutable access to the orbit camera so the window proc can drive it from mouse
    // input (g_renderer.Camera().Orbit(...) / .Zoom(...) / .Pan(...)).
    OrbitCamera& Camera() { return cam_; }

    // Re-frames the camera on the current asset's AABB and recomputes near/far — the
    // one-call entry point for the Reset View button (FR25 / Journey 2 recovery).
    void ResetCamera();

    // Story 6.5.3 light-tool setters/getters. These mutate the light members that
    // RenderFrame already pushes to u_lightColor/u_lightDir EVERY frame (6.5.1) — so
    // the light tool only nudges these on a user action and the next ~66 Hz frame
    // applies them (zero new per-frame work, D2 zero-alloc preserved). The getters
    // seed the colour picker / position control to the current value. SetLightDir
    // normalizes (u_lightDir is the direction TOWARD the light, the shader's L) and
    // ignores a degenerate near-zero vector so normalize() never yields NaN. [Review][Patch]
    void SetLightColor(const glm::vec3& c) { light_color_ = c; }
    void SetLightDir(const glm::vec3& d)   { if (glm::dot(d, d) > 1e-12f) light_dir_ = glm::normalize(d); }
    glm::vec3 LightColor() const { return light_color_; }
    glm::vec3 LightDir()   const { return light_dir_; }

    // Live lighting-quality knobs (light tool, 6.5.x polish — Antho's "render looks flat/cheap
    // vs Mixamo" feedback). ambient = fill amount (lower = more form contrast); spec strength =
    // specular sheen that sculpts the surface; normal strength = relief boost on the normal map
    // (1 = as-authored, higher exaggerates wrinkles/pores). All pushed to uniforms each frame;
    // the getters seed the sliders. Clamped to sane ranges so a stray value can't break shading.
    void  SetAmbient(float a)        { ambient_        = (a < 0.0f) ? 0.0f : (a > 1.0f ? 1.0f : a); }
    void  SetSpecStrength(float s)   { spec_strength_  = (s < 0.0f) ? 0.0f : (s > 4.0f ? 4.0f : s); }
    void  SetNormalStrength(float n) { normal_strength_= (n < 0.0f) ? 0.0f : (n > 4.0f ? 4.0f : n); }
    float Ambient()        const { return ambient_; }
    float SpecStrength()   const { return spec_strength_; }
    float NormalStrength() const { return normal_strength_; }

    // Story 6.5.4 shadow-quality tool (Off/Low/Mid/High). The setter (RE)ALLOCATES the
    // shadow map on a quality CHANGE only (cold path) — Off frees it and skips the depth
    // pass, the other levels size the depth texture (512/1024/2048) — so the per-frame
    // path stays allocation-free (D2). Requires a current GL context (it creates/frees a
    // depth texture + FBO). A shadow-map allocation failure is non-fatal: it LogWarns and
    // falls back to Off, leaving the viewport (and the floor) running (AR17). RenderFrame
    // reads the current quality each frame. (No same-named getter: a member function named
    // ShadowQuality() would hide the namespace-scope enum type and break the out-of-line
    // definition; the tool seeds its 4-way selector from its own default, which matches the
    // Mid default below.)
    void SetShadowQuality(ShadowQuality q);

    // Story 6.5.4 (post-gate, Antho): show/hide the ground plane. Default on. Hiding the
    // floor also skips the shadow depth pass (the floor is the only shadow receiver, so
    // there is nothing to cast onto — RenderFrame gates the pass on this too). Session-only.
    void SetFloorVisible(bool v) { floor_visible_ = v; }

    // Story 6.5.5 — Normal-maps render-quality toggle (FR52). Defaults ON (quality); a weak PC
    // turns it off to recover frame time. Session-only, mirrors SetFloorVisible. AND-gated into
    // the per-material u_hasNormalMap flag in RenderFrame — NO GLSL change, the shader already
    // keeps the geometric normal when the flag is 0 (6.5.1 AC4). Off → flatter relief on every
    // material; on → restores it, immediately.
    void SetNormalMapsEnabled(bool v) { normal_maps_on_ = v; }

    // Story 6.5.6 — selectable MSAA level (FR52), replacing 6.5.5's on/off. The value is the
    // requested sample count: 0 == Off (renders straight to the window, zero offscreen cost),
    // 2/4/8 == that many samples in the offscreen multisample colour FBO that RenderFrame
    // resolves to the window. The (re)allocation happens lazily in RenderFrame on a level/size
    // CHANGE only (cold path) — so this setter just stores the value (mirrors SetFloorVisible;
    // no GL work, no current-context requirement). The caller clamps to GL_MAX_SAMPLES before
    // setting (an unclamped count would make glRenderbufferStorageMultisample raise
    // GL_INVALID_VALUE). A failed allocation falls back non-fatally to Off (AR17).
    void SetMsaaSamples(int s) { msaa_samples_ = s; }

    // Releases GL resources (including the held Asset's buffers). Must run while
    // the GL context is current. Safe to call more than once.
    void Shutdown();

private:
    // Builds the flat-colour floor program + grid mesh (Story 6.5.4). Non-fatal: on a
    // shader-compile failure it LogWarns and leaves floor_program_ empty so the floor
    // draw is skipped while the rest of the viewport runs (AR17). Called once from Init.
    void BuildFloor();
    // Draws the always-on ground plane + grid for the current asset, receiving the cast
    // shadow via PCF. No-op if the floor program failed to build. Switches to
    // floor_program_/floor_vao_; the caller re-binds the mesh program after.
    void DrawFloor();
    // (Re)allocates the shadow-map depth texture + FBO at `size`x`size`. Returns false on
    // any GL failure (caller falls back to Off). Cold path (SetShadowQuality / Init only).
    bool AllocShadowMap(int size);
    // (Re)allocates the offscreen MSAA colour+depth renderbuffers + FBO at width x height with
    // `samples` samples (Story 6.5.6). Returns false on any GL failure (caller falls back to Off,
    // AR17). Cold path only — RenderFrame calls it on a level/size change, never per frame (D2).
    bool AllocMsaaTargets(int w, int h, int samples);
    // Re-points the shared VAO's attributes (pos/normal/uv + bone ids/weights) at this
    // mesh's VBO/IBO — the per-mesh setup shared by the depth pass and the visible pass.
    void BindMeshAttribs(const SceneMesh& mesh);

    GpuProgram     program_;
    GpuVertexArray vao_;
    Asset          asset_;
    OrbitCamera    cam_;         // D14 orbit state; the view is derived from it each frame

    // D13 skinning palette + its forward-pass scratch. Sized ONCE in SetAsset to the
    // skeleton's bone count (cold load path) and reused every frame by ComputePose, so
    // RenderFrame stays allocation-free (D2). Empty for a static asset.
    std::vector<glm::mat4> palette_;
    std::vector<glm::mat4> pose_scratch_;

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
    int u_bones_         = -1;   // mat4[128] skinning palette (D13); -1 if driver elides
    int u_skinned_       = -1;   // 0 → static Epic 2 path (bit-identical), 1 → skin in shader

    // Story 6.5.1 — lighting + normal-map uniforms. All non-fatal on -1 (driver may
    // dead-strip a uniform it proves dead); only u_mvp is required.
    int u_light_color_   = -1;
    int u_light_dir_     = -1;
    int u_ambient_       = -1;
    int u_normal_map_    = -1;   // sampler2D bound to texture unit 1
    int u_has_normal_map_ = -1;  // 0 → geometric normal (asset carries no normal map)
    int u_spec_strength_   = -1; // live specular strength (light tool)
    int u_normal_strength_ = -1; // live normal-map relief boost (light tool)

    // Light defaults, stored so Story 6.5.3's light tool can drive them later. Set once
    // per frame in RenderFrame; the 6.5.3 Dear ImGui tool UI drives light_color_ (colour
    // picker) and light_dir_ (circular light-position pad) on user action.
    glm::vec3 light_color_{1.0f, 1.0f, 1.0f};
    glm::vec3 light_dir_  {glm::normalize(glm::vec3(0.4f, 0.9f, 0.5f))};  // prior hardcoded dir
    // Lighting-quality defaults nudged toward more contrast/relief after Antho's "looks flat
    // vs Mixamo" feedback (the 6.5.1 gate used ambient 0.35 / spec 0.35). All three are live-
    // adjustable via the light tool so Antho dials the final look in-Reaper.
    // Defaults Antho dialed in-Reaper against the Mixamo "Vampire" reference (2026-06-28):
    // very low fill + strong specular + boosted relief for punchy, sculpted skin. Still
    // live-adjustable via the light tool.
    float     ambient_         = 0.05f;  // near-zero fill → strong form contrast
    float     spec_strength_   = 1.50f;  // pronounced sheen sculpting skin/cloth
    float     normal_strength_ = 1.50f;  // boosted normal-map relief (wrinkles/pores read)

    // Story 6.5.4 — always-on floor: a flat-colour ground plane + grid in its own minimal
    // program (NOT the lit material shader). Built ONCE in Init (cold path), scaled to the
    // model AABB per frame, drawn opaque under the model. Receives the cast shadow via PCF.
    GpuProgram     floor_program_;
    GpuVertexArray floor_vao_;
    GpuBuffer      floor_vb_;          // solid quad (4 verts) then grid lines, one buffer
    GLsizei        gridVertCount_ = 0; // grid-line vertex count (drawn after the quad)
    bool           floor_visible_ = true;  // post-gate floor on/off toggle (Antho); default on
    // Story 6.5.5/6.5.6 render-quality levers (FR52). normal_maps_on_ (default on) AND-gates the
    // per-material u_hasNormalMap flag (one uniform value, no GLSL). msaa_samples_ (default 4×,
    // clamped to GL_MAX_SAMPLES at startup) selects the offscreen multisample colour FBO's sample
    // count: 0 == Off (draw straight to the window). Session-only — a fresh viewer opens here.
    bool           normal_maps_on_ = true;
    int            msaa_samples_   = 4;
    int u_floor_mvp_         = -1;
    int u_floor_model_       = -1;
    int u_floor_color_       = -1;
    int u_floor_light_color_ = -1;
    int u_floor_light_space_ = -1;
    int u_floor_shadow_map_  = -1;
    int u_floor_shadow_on_   = -1;
    int u_floor_pcf_radius_  = -1;
    int u_floor_shadow_texel_ = -1;

    // Story 6.5.4 — real-time shadow map. shadow_fbo_ is the TRANSIENT offscreen depth FBO
    // bound only during the depth pass (FB0 restored after — see the header comment);
    // shadow_depth_tex_ is its GL_DEPTH_COMPONENT24 attachment the floor samples. Allocated
    // on a quality change only (cold path); shadow_map_size_ is the current allocation
    // (0 == none / Off). light_space_ is rebuilt each frame from light_dir_ so the 6.5.3
    // light pad sweeps the shadow (AC3).
    GpuFramebuffer shadow_fbo_;
    GpuImage       shadow_depth_tex_;
    ShadowQuality  shadow_quality_ = ShadowQuality::Mid;  // default Mid (AC2)
    int            shadow_map_size_ = 0;
    glm::mat4      light_space_{1.0f};

    // Story 6.5.6 — offscreen MULTISAMPLE colour FBO that the scene renders into when MSAA is on,
    // then blit-resolves to the window (FB0). msaa_color_rb_ is a GL_RGBA8 multisample renderbuffer
    // (NOT sRGB — the shader already encodes sRGB on its final write, 6.5.1; an sRGB RB would
    // double-encode); msaa_depth_rb_ is GL_DEPTH_COMPONENT24 (matches the scene depth precision).
    // (Re)allocated on a level/resize CHANGE only (cold path, mirrors AllocShadowMap), so the
    // per-frame path stays allocation-free (D2). The alloc cache lets RenderFrame skip realloc
    // when nothing changed. All freed at Off (no offscreen cost, AC4). A failed alloc → Off (AR17).
    GpuFramebuffer  msaa_fbo_;
    GpuRenderbuffer msaa_color_rb_;
    GpuRenderbuffer msaa_depth_rb_;
    int             msaa_alloc_w_ = 0, msaa_alloc_h_ = 0, msaa_alloc_samples_ = 0;
};

}  // namespace rav

#endif  // _WIN32
