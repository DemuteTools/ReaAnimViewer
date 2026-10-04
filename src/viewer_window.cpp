// SPDX-License-Identifier: MIT
//
// Viewer window: a dockable Win32 WS_CHILD window owning a WGL context that
// renders a scene DIRECTLY into the window framebuffer (no offscreen FBO) and
// presents via SwapBuffers, driven by an independent ~66 Hz Win32 timer so the
// viewport exceeds Reaper's ~30 Hz UI loop (Spike 0 / Story 1.2). The child is
// handed to Reaper's native docker via DockWindowAddEx (Story 1.3).
//
// HIDE HANDLING (proven by a Story 1.3 message trace): when a docked panel is
// closed via its tab X, Reaper hides an *ancestor* (the docker host) — it sends
// our window NO message at all (no WM_DESTROY/WM_CLOSE/WM_SHOWWINDOW/
// WM_WINDOWPOSCHANGED; a dialog and a screenset both changed nothing) and does not
// even remove us from the docker. Worse, a close is INDISTINGUISHABLE from a
// transient hide (minimize, another docker tab active), so trying to destroy the
// window on a hide misfired and corrupted it during minimize/restore. So the render
// timer does NOT destroy on hide: it renders while the window is visible and PARKS
// while hidden (no SwapBuffers / no fps / no GPU — nothing runs in the background).
// The window is destroyed only by the toggle action (Action while open) or unload —
// the paths where WE know it is a real close. Init-failure hardening is Story 1.4;
// mesh loading is Epic 2.

#include "viewer_window.h"

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM — unpack WM_MOUSEMOVE coords
#include <cmath>       // std::sin/cos/asin/atan2/sqrt — light azimuth/elevation ↔ direction
#include <cfloat>      // FLT_MAX (spec 11-fb-11: the menu's height cap)
#include <algorithm>   // std::min/max — the load message's wrap width
#include <exception>   // std::exception — the no-throw host boundary (AR18)
#include <filesystem>  // issue #1: the item folder for "Locate textures..."
#include <string>
#include <unordered_set>  // g_noticed_paths
#include <vector>

#include "asset_cache.h"      // Story 11-2: share the parse with the video FX
#include "asset_loader.h"
#include "console_log.h"
#include "video_fx_track.h"   // RefreshVideoFxPictures after a display change
#include "display_settings.h" // Story 11-2: the video FX draws with the viewer's settings
#include "gl_loader.h"   // LoadGlFunctions + modern-GL pointers; pulls in <gl/GL.h>
#include "overlay_icons.h"    // kIcon_menu/light/color — Antho's SVGs rasterized to RGBA (Story 6.5.3 rev)
#include "pcm_source_anim.h"  // GetCurrentAnimItem — the transport→current-item query (Story 4.3)
#include "rav_version.h"      // RAV_DISPLAY_VERSION — menu footer + copied error log header
#include "renderer.h"
#include "shortcuts.h"         // spec 11-fb-3: the viewer's keys, rebindable
#include "shortcuts_ui.h"      // spec 11-fb-3: the keyboard icon + Shortcuts popup
#include "texture_locate.h"    // issue #1: "Locate textures..." (folder picker + copy)
#include "gpu_resources.h"    // Story 11-4: the Video view's render target
#include "ui_theme.h"         // Story 11-4: the DM-XYZ-Pad theme over Dear ImGui
#include "video_view.h"       // Story 11-4: Video view + Video panel model
#include "video_view_ui.h"    // Story 11-4: their ImGui widgets

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F   // GL 1.2; Windows <gl/GL.h> is 1.1 and may omit it
#endif

// Dear ImGui (vendored, Story 6.5.3 rev) — the in-viewport tool UI. Rendered into OUR
// GL context after the 3D scene, so it is a true overlay (no flicker, no extra window,
// no ReaImGui runtime dependency). Win32 backend feeds input via the WndProc handler;
// the OpenGL3 backend draws the widget geometry.
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_opengl3.h>

// imgui_impl_win32.h intentionally comments out (inside an `#if 0`) the WndProc handler
// declaration so the header doesn't drag in <windows.h>; we forward-declare it here (we
// already have <windows.h>) so WindowProc can pass messages to the ImGui Win32 backend.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace rav {
namespace {

constexpr wchar_t kWindowClassName[] = L"ReaAnimViewer.ViewerWindow";
constexpr wchar_t kWindowTitle[]     = L"ReaAnimViewer";

// Docker handles: kDockName is the tab caption Reaper shows; kDockIdent is the
// stable persistence key Reaper uses to remember the dock position across
// sessions — it must NOT change between builds or saved layouts are orphaned
// (confirmed strings, Story 1.3 Resolved Decisions).
constexpr char kDockName[]  = "ReaAnimViewer";
constexpr char kDockIdent[] = "ReaAnimViewer.Viewer";

constexpr int kInitialWidth  = 800;
constexpr int kInitialHeight = 600;

// ~66 Hz: SetTimer's ~15.6 ms floor yields ~64 fps, comfortably > Reaper's
// ~30 Hz extension-timer cap. The render loop must NOT ride rec->Register("timer")
// (that rides the UI loop); a thread timer dispatched by Reaper's main pump keeps
// rendering on the main thread (AR18) while beating the 30 Hz cadence.
//
// KNOWN LIMITATION: rendering is pump-driven, so the viewport pauses during host
// modal sub-loops (an open menu, a modal dialog) and resumes when they exit. This
// is inherent to a single-thread render loop, not the NULL-hwnd timer choice (a
// window-bound timer stalls in the same loops); a dedicated render thread would
// be the only real fix, which is out of scope for the MVP viewport.
constexpr UINT kFrameTimerMs = 15;

HWND  g_hwnd  = nullptr;
HDC   g_hdc   = nullptr;
HGLRC g_hglrc = nullptr;
bool  g_class_registered = false;
REAPER_PLUGIN_HINSTANCE g_class_hinst = nullptr;  // remembered so unload can UnregisterClass with the correct module

Renderer g_renderer;

// Story 4.3 transport-drive state. The displayed pose is derived from the playhead
// every frame, never stored (architecture.md:305): g_display_time is just the last
// transport-driven animTime, kept so the frame FREEZES when the playhead leaves the
// span (AC3) instead of jumping. g_current_anim_path is the source path of the asset
// currently loaded FROM an item (empty until the first item drives the view) — the
// cheap string-compare gate that makes us reload only on a scrub onto a DIFFERENT
// item, never every frame. g_transport_driven latches true the first time any RAV
// item is current and STAYS true (transport mode is sticky — it never reverts to the
// looping fixture, which would thrash reloads and contradict AC3's "hold").
std::string g_current_anim_path;
double      g_display_time     = 0.0;
bool        g_transport_driven = false;

UINT_PTR g_timer_id = 0;
int g_client_w = kInitialWidth;
int g_client_h = kInitialHeight;

// Frame clock + present-rate fps (QueryPerformanceCounter; the timer cadence is
// only the lower bound — fps is measured from actual SwapBuffers calls).
LARGE_INTEGER g_qpc_freq{};
LARGE_INTEGER g_qpc_start{};
LARGE_INTEGER g_fps_last{};
// Story 6.5.8 — per-frame clock for the ViewCube snap tween. QuadPart==0 means "no prior
// tick" (first frame, or just resumed from a paused panel) → that frame's dt is forced to 0
// so a long hidden gap can't teleport the tween.
LARGE_INTEGER g_last_tick{};
int           g_frame_count = 0;
// Story 6.5.5 — the FPS value the on-canvas readout shows. The measurement (QPC + frame
// count + 1-Hz roll-up) has always run (6.5.2 silenced only the LOG, not the math); this
// just STORES the computed rate so the overlay can read it. Updated in the RenderTick roll-up.
float         g_fps = 0.0f;

// True while the panel is hidden and the render loop is parked — see FrameTimerProc.
bool g_render_paused = false;

// Mouse-drag camera state (D14). A drag is right-button=orbit / middle-button=pan;
// SetCapture keeps reporting moves even if the cursor leaves the window, and
// WM_CAPTURECHANGED resets DragMode so a yanked capture can't leave a phantom drag.
enum class DragMode { None, Orbit, Pan };
DragMode g_drag = DragMode::None;
int g_last_x = 0;
int g_last_y = 0;

// Story 6.5.3 (rev) — Dear ImGui tool UI state. A frameless hamburger MENU (Antho's
// icons) that show/hides a list of tools, drawn as a true overlay in our GL frame. The
// light's "position around the origin" is held here as azimuth/elevation (radians) and
// converted to a direction with the SAME spherical form as OrbitCamera::Eye (camera.h,
// Y-up AR9); the colour is RGB the picker edits. Both push to the renderer on change.
bool  g_imgui_ready    = false;     // true once ImGui + its backends are initialized
bool  g_menu_open      = false;     // the hamburger toggles the option list
float g_light_azimuth  = 0.0f;      // seeded from the renderer's default direction
float g_light_elevation = 0.0f;
float g_light_color[3] = { 1.0f, 1.0f, 1.0f };

// Live lighting-quality sliders (light tool, 6.5.x polish — Antho's "flat/cheap vs Mixamo"
// feedback). Seeded from the renderer's defaults in StartRendering; each pushes to the renderer.
float g_ambient        = 0.5f;
float g_spec_strength  = 1.50f;
float g_normal_strength = 1.50f;

// Story 6.5.4 shadow-quality selection (0=Off, 1=Low, 2=Mid, 3=High). Mid (2) matches the
// renderer's default ShadowQuality::Mid so the UI and the render agree at startup without a
// getter. Session-only — a fresh viewer opens at Mid (no persistence, like the light/camera).
int g_shadow_quality = 2;

// Story 6.5.4 (post-gate, Antho) — floor on/off. Default on, matching the renderer default.
bool g_floor_visible = true;
// Epic 9 — floor grid cell size in metres (1 or 10). Default 1 m, matching the renderer's
// default grid_step_m_. Session-only, like the other Ground/Shadow levers.
int g_grid_step_m = 1;

// Story 6.5.5/6.5.6 — Performance section levers (FR52/FR53). Default to the quality end (Antho's
// request) and seed the renderer's matching defaults in StartRendering, like g_floor_visible.
// Session-only.
bool g_normal_maps_on   = true;   // FR52 — AND-gate the per-material normal-map flag
bool g_fps_overlay_on   = true;   // FR53 — top-right on-canvas FPS readout (default on)
bool g_preview_lag_on   = true;   // Video view caption: REAPER catch-up (default on, session only)
// Story 6.5.6 — selectable MSAA level (FR52), replacing 6.5.5's on/off. g_msaa_level is the
// selected sample count (0 = Off / 2 / 4 / 8; default 4×, clamped to the GPU max at startup);
// g_msaa_max is GL_MAX_SAMPLES, queried once the context is current in StartRendering so an
// option above the GPU's max is shown disabled (AR17 — Off + any level <= max stay enabled).
int  g_msaa_level = 4;
int  g_msaa_max   = 0;

// Story 6.5.5 — minimal on-canvas load-failure indication (AC7), rehoming the signal 6.5.2
// silenced. Set where the load-failure LogError fires; shown as a brief transient overlay
// while ElapsedSeconds() < g_load_msg_until, then it just disappears. No popup (AR16).
// It speaks the user's language: on a failure, LoadResult::hint (what is wrong with the
// file and what to fix in the export); on a successful load with problems, the loader's
// notices (missing textures, no animation...). Technical detail stays in the copyable log.
// Long enough to read and reach its "Copy details" button, and held while hovered.
// The message is also the current item's STATUS: it stays set until the next load (a
// clean load clears it), and the bottom-left status icon re-shows it on hover after the
// transient pop-up has gone (StatusIconWidget).
constexpr double kLoadMessageSeconds = 10.0;
std::string              g_load_msg_title;        // "" = no status (last load was clean)
std::vector<std::string> g_load_msg_lines;
bool                     g_load_msg_error = false;  // red (blocking) vs amber (notices)
double                   g_load_msg_until = 0.0;    // transient pop-up visible until then
// Issue #1: a "Locate textures..." outcome, shown above the notices (green). With no
// notices left (all textures found) the message is only this report: g_load_msg_info,
// green title, transient pop-up only (no status icon: the item has no problem).
std::vector<std::string> g_load_msg_report;
bool                     g_load_msg_info = false;
// Issue #1: the current item's texture files not found (LoadResult::missing_texture_files)
// and its path: the "Locate textures..." button shows while the list is non-empty, and
// copies into that path's folder. Set with the load status, cleared with it.
std::string              g_missing_tex_item;
std::vector<std::string> g_missing_tex_files;
// The button posts kMsgLocateTextures (the folder picker runs a modal loop: window
// procedure only, never inside the ImGui frame); this flag stops a second post meanwhile.
constexpr UINT kMsgLocateTextures = WM_APP + 0x32;
bool                     g_locate_pending = false;
// A copy reloads the item (next tick); its report waits here to be shown with the
// reloaded item's status. Dropped if another item loads first.
std::string              g_locate_report_item;
std::vector<std::string> g_locate_report_lines;
// Files whose notices already POPPED UP since the panel opened: a file with known
// problems must not pop them up on every scrub back onto it (the status icon still
// shows them). Failures always pop up. Cleared in StopRendering, so reopening the
// panel pops them up again.
std::unordered_set<std::string> g_noticed_paths;

// Bottom-left status icon (red = the current item failed to load, amber = it loaded with
// problems). Hovering it opens the message panel; g_status_panel_until keeps that panel
// open for a short grace period so the mouse can travel from the icon to its button.
constexpr float  kStatusIconSize    = 22.0f;
constexpr float  kStatusIconMargin  = 12.0f;
constexpr double kStatusPanelGrace  = 0.3;
double           g_status_panel_until = 0.0;

// Copyable error log — the GL strings head the clipboard text (probed in StartRendering)
// and g_copied_until drives the brief "Copied!" button feedback.
std::string g_gl_version;
std::string g_gl_renderer;
double      g_copied_until = 0.0;

// Icon textures (Antho's SVGs, uploaded once at ImGui init; freed in StopRendering).
GLuint g_icon_menu   = 0;
GLuint g_icon_light  = 0;
GLuint g_icon_color  = 0;
GLuint g_icon_shadow = 0;
GLuint g_icon_ground = 0;
GLuint g_icon_performance = 0;  // Story 6.5.5 — Antho's Performance-section icon
GLuint g_icon_keyboard = 0;     // spec 11-fb-3 — the Shortcuts popup's icon (Material "keyboard")

// Spec 11-fb-3 — the key the accelerator hook last claimed for the viewer (its WM_CHAR /
// WM_KEYUP follow it to the viewer instead of REAPER), and the claimed key whose WM_SYSKEYUP
// was handed to us (WindowProc swallows it, so F10 / Alt never reach the menu bar).
KeyRouteState g_key_route;

// Spec 11-fb-3 — focus lost or panel parked: no recording the user cannot see (its keys
// would be swallowed), no stale claim.
void DropKeyClaims()
{
    if (ShortcutRecordingId() >= 0) CancelShortcutRecording();
    g_key_route = KeyRouteState{};
}

constexpr float kPiF    = 3.14159265f;
constexpr float kHalfPi = 1.57079633f;

// Story 6.5.8 — navigation cube (ViewCube) feel knobs. Gate-tunable like the orbit
// sensitivities (camera.h) — judged in-Reaper on Windows, not on the Linux dev box.
// kNavCubeSize is the square hit/draw area edge (px); kNavCubeMargin is the gap from the
// viewport's bottom-right corner. The 3D cube is drawn + picked inside that square.
constexpr float kNavCubeSize   = 96.0f;
constexpr float kNavCubeMargin = 12.0f;

// Story 11-4 — Video view. The frame is rendered into its own target at the frame's size
// (same aspect as the FX output, so the same framing), then copied into the frame rect of
// the window. Session state; freed with the GL context in StopRendering.
constexpr UINT kMsgRunVideoCommands = WM_APP + 0x31;  // runs the Video view's queued writes
GpuFramebuffer  g_video_fbo;
GpuRenderbuffer g_video_color_rb;
GpuRenderbuffer g_video_depth_rb;
int             g_video_target_w = 0;
int             g_video_target_h = 0;
VideoFrameRect  g_video_frame;              // this frame's layout (Video view only)
bool            g_drag_video = false;       // the current drag edits the video camera
bool            g_video_post_pending = false;
// True when the asset the renderer holds is the one g_current_anim_path names (a failed
// load keeps the previous asset up, whose bounds then belong to another file).
bool            g_current_load_ok = false;

ImTextureID IconTex(GLuint t) { return static_cast<ImTextureID>(t); }  // GLuint → ImU64

glm::vec3 LightDirFromAngles(float azim, float elev)
{
    const float cp = std::cos(elev);
    return glm::vec3(cp * std::sin(azim), std::sin(elev), cp * std::cos(azim));
}

GLuint UploadIconTexture(const unsigned char* rgba, int w, int h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    if (!t) return 0;
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

void DrawToolUi();  // defined below; called from RenderTick before SwapBuffers

double ElapsedSeconds()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - g_qpc_start.QuadPart) / double(g_qpc_freq.QuadPart);
}

// File name for the on-canvas message (the full path is in the copyable log).
std::string FileNameOf(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Sets the current item's load status (see g_load_msg_title) and, if `pop_up`, also shows
// it as the transient top-centre message.
void SetLoadMessage(bool error, std::string title, std::vector<std::string> lines, bool pop_up)
{
    g_load_msg_error = error;
    g_load_msg_info  = false;
    g_load_msg_title = std::move(title);
    g_load_msg_lines = std::move(lines);
    g_load_msg_report.clear();
    g_load_msg_until = pop_up ? ElapsedSeconds() + kLoadMessageSeconds : 0.0;
}

void ClearLoadMessage()
{
    g_load_msg_title.clear();
    g_load_msg_lines.clear();
    g_load_msg_report.clear();
    g_load_msg_info  = false;
    g_load_msg_until = 0.0;
}

// Puts a pasteable bug-report block on the clipboard: the GPU/driver the viewer runs on,
// then the kept warn/error lines (console_log.h). Called from an ImGui button, so it runs
// inside the frame where ImGui's Win32 clipboard hook is live.
void CopyErrorLogToClipboard()
{
    std::string text = "ReaAnimViewer error log (version " RAV_DISPLAY_VERSION ")\n";
    text += "OpenGL: " + g_gl_version + " on " + g_gl_renderer + "\n";
    text += HasRecentLog() ? RecentLogText() : std::string("(no errors logged)\n");
    ImGui::SetClipboardText(text.c_str());
    g_copied_until = ElapsedSeconds() + 2.0;
}

// "a, b, c" for a result line.
std::string JoinNames(const std::vector<std::string>& names)
{
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) out += (i ? ", " : "") + names[i];
    return out;
}

// Issue #1 — the "Locate textures..." button (kMsgLocateTextures, window procedure: the
// folder picker runs a modal loop). The user picks a folder; the current item's missing
// texture files found there by name are copied next to the item's file (never
// overwriting), then the viewer and the video FX read the file again. Cancel = nothing.
void RunLocateTextures()
{
    if (g_missing_tex_files.empty() || g_missing_tex_item.empty()) return;
    // Copies: the modal picker lets the render tick run (it may load another item).
    const std::string              item  = g_missing_tex_item;
    const std::vector<std::string> names = g_missing_tex_files;

    std::string folder;
    // Owned by the top-level window: the viewer may be a docked child of REAPER's docker.
    HWND owner = g_hwnd ? GetAncestor(g_hwnd, GA_ROOT) : nullptr;
    if (!owner) owner = g_hwnd;
    if (!PickFolder(owner, folder)) return;   // cancelled

    std::string dest_dir;
    try {
        dest_dir = std::filesystem::u8path(item).parent_path().u8string();
    } catch (...) {
        return;
    }
    const LocateResult res = LocateTextures(dest_dir, names, folder);
    LogInfo("locate textures in %s -> %s: %zu copied, %zu already there, %zu failed, %zu not found",
            folder.c_str(), dest_dir.c_str(), res.copied.size(), res.already_present.size(),
            res.failed.size(), res.still_missing.size());

    std::vector<std::string> lines;
    if (!res.copied.empty())
        lines.push_back(std::to_string(res.copied.size()) +
                        (res.copied.size() == 1 ? " texture copied" : " textures copied") +
                        " next to the model: " + JoinNames(res.copied));
    if (!res.already_present.empty())
        lines.push_back("Already next to the model (not replaced): " +
                        JoinNames(res.already_present));
    for (const std::string& f : res.failed) {
        lines.push_back("Could not copy " + f + " to " + dest_dir);
        LogWarn("locate textures: could not copy %s to %s", f.c_str(), dest_dir.c_str());
    }
    if (res.copied.empty() && res.already_present.empty() && res.failed.empty())
        lines.push_back("No missing texture found in " + folder);
    else if (!res.still_missing.empty())
        lines.push_back("Not found in " + folder + ": " + JoinNames(res.still_missing));

    if (!res.copied.empty() || !res.already_present.empty()) {
        // Read the file again with its new textures (copied now, or put there by hand
        // since the load): the video FX's cached parse is dropped (its date and size did
        // not change), and the viewer's reload gate re-arms (next tick). The report is
        // shown with the reloaded item's status.
        EvictCpuAsset(item);
        RefreshVideoFxPictures();
        g_locate_report_item  = item;
        g_locate_report_lines = std::move(lines);
        if (g_current_anim_path == item) g_current_anim_path.clear();
        return;
    }
    // Nothing copied or present: no reload; the outcome shows above the item's notices.
    if (g_missing_tex_item == item && !g_load_msg_title.empty()) {
        g_load_msg_report = std::move(lines);
        g_load_msg_until  = ElapsedSeconds() + kLoadMessageSeconds;
    }
}

void DestroyGLContext()
{
    if (g_hglrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(g_hglrc);
        g_hglrc = nullptr;
    }
    if (g_hdc) {
        // g_hwnd is still null if StartRendering bails inside WM_CREATE (it is
        // assigned only after CreateWindowExW returns), so recover the owning
        // HWND from the DC itself; always drop g_hdc so it can't dangle.
        if (HWND owner = g_hwnd ? g_hwnd : WindowFromDC(g_hdc))
            ReleaseDC(owner, g_hdc);
        g_hdc = nullptr;
    }
}

bool CreateGLContextFor(HWND hwnd)
{
    g_hdc = GetDC(hwnd);
    if (!g_hdc) return false;

    // Story 6.5.6 — the window is SINGLE-SAMPLE; MSAA now lives entirely in the offscreen
    // multisample FBO the renderer resolves to this window (the 6.5.5 wglChoosePixelFormatARB
    // multisample bootstrap was reverted — SetPixelFormat is one-shot per HDC and the window is
    // never recreated, so a baked-in multisample format could never change level live, and
    // glDisable(GL_MULTISAMPLE) on the default framebuffer is driver-ignored). The legacy
    // single-sample ChoosePixelFormat/SetPixelFormat path below is all the window needs.
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.cDepthBits   = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType   = PFD_MAIN_PLANE;

    const int pf = ChoosePixelFormat(g_hdc, &pfd);
    if (!pf || !SetPixelFormat(g_hdc, pf, &pfd)) {
        ReleaseDC(hwnd, g_hdc); g_hdc = nullptr;
        return false;
    }

    g_hglrc = wglCreateContext(g_hdc);
    if (!g_hglrc) {
        ReleaseDC(hwnd, g_hdc); g_hdc = nullptr;
        return false;
    }

    return true;
}

// Vsync OFF (interval 0): the ~66 Hz timer paces presentation, and an uncapped
// present rate makes the >=60 fps headroom *measurable* in the console (AC wants
// the rate observed, not assumed). Story 1.3 keeps vsync OFF through Epic 1 (a
// confirmed decision) so the validator can re-confirm NFR-P1 *while docked* at a
// real docked size — tear-free vsync-on is a later-polish flip, not Epic 1's job.
void TrySetVsync(int interval)
{
    typedef BOOL(WINAPI * PFNSWAPINTERVAL)(int);
    if (auto swap = reinterpret_cast<PFNSWAPINTERVAL>(wglGetProcAddress("wglSwapIntervalEXT")))
        swap(interval);  // absence is not a failure: it just means vsync can't be toggled
}

// Story 11-4 — (re)allocates the Video view's render target (colour + depth, single
// sample: the renderer's own MSAA target resolves into it). Cold path: only on a size
// change. False when the GPU refuses it (the frame then stays empty); a refused size is
// not retried (nor logged again) until the frame size changes.
bool EnsureVideoTarget(int w, int h)
{
    if (g_video_target_w == w && g_video_target_h == h) return g_video_fbo.get() != 0;
    g_video_fbo = GpuFramebuffer();
    g_video_color_rb = GpuRenderbuffer();
    g_video_depth_rb = GpuRenderbuffer();
    g_video_target_w = w;
    g_video_target_h = h;

    GpuRenderbuffer color;
    glGenRenderbuffers(1, color.addr());
    if (!color.get()) return false;
    glBindRenderbuffer(GL_RENDERBUFFER, color.get());
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 0, GL_RGBA8, w, h);
    GpuRenderbuffer depth;
    glGenRenderbuffers(1, depth.addr());
    if (!depth.get()) return false;
    glBindRenderbuffer(GL_RENDERBUFFER, depth.get());
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 0, GL_DEPTH_COMPONENT24, w, h);
    GpuFramebuffer fbo;
    glGenFramebuffers(1, fbo.addr());
    if (!fbo.get()) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo.get());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color.get());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth.get());
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LogWarn("Video view: render target %dx%d refused by the GPU, the frame stays empty", w, h);
        return false;
    }
    g_video_color_rb = std::move(color);
    g_video_depth_rb = std::move(depth);
    g_video_fbo = std::move(fbo);
    return true;
}

// Story 11-5 -- the shot strip's band at the bottom of the view area (Video view with an
// active FX), 0 otherwise. The frame, the ViewCube and the status icon stay above it.
float VideoStripBand()
{
    // Spec 11-fb-11: the strip's height is the user's (its top edge drags it).
    return VideoShotStripVisible() ? VideoStripHeight(static_cast<float>(g_client_h)) + 2.0f * kVideoStripGap : 0.0f;
}

// The bottom of the view area (client height minus the strip band).
float ViewBottom()
{
    return std::max(1.0f, static_cast<float>(g_client_h) - VideoStripBand());
}

void ReleaseVideoTarget()
{
    g_video_fbo = GpuFramebuffer();
    g_video_color_rb = GpuRenderbuffer();
    g_video_depth_rb = GpuRenderbuffer();
    g_video_target_w = 0;
    g_video_target_h = 0;
}

// Story 11-4 — Video view: the window shows the FX's picture inside a frame at the output
// aspect. Outside the frame = the theme's background. The free camera is swapped out for
// the video camera only around the draw, so RAV view finds it exactly as it was left.
void RenderVideoView(float anim_time)
{
    const VideoViewModel& m = GetVideoViewModel();
    const float area_w = static_cast<float>(g_client_w) - VideoPanelFootprint(g_client_w);
    g_video_frame = ComputeVideoFrameRect(area_w, ViewBottom(), VideoTopBand(), m.out_w, m.out_h);
    const int fx = static_cast<int>(g_video_frame.x);
    const int fw = static_cast<int>(g_video_frame.w);
    const int fh = static_cast<int>(g_video_frame.h);
    const int fy = g_client_h - (static_cast<int>(g_video_frame.y) + fh);  // GL rows count from the bottom

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_client_w, g_client_h);
    const ImVec4 outside = ui::Col(ui::kBg);
    glClearColor(outside.x, outside.y, outside.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    OrbitCamera video_cam;
    if (fw >= 1 && fh >= 1 && VideoViewCamera(&video_cam) && EnsureVideoTarget(fw, fh)) {
        const OrbitCamera free_cam = g_renderer.Camera();
        g_renderer.Camera() = video_cam;
        g_renderer.RenderFrame(anim_time, /*loop=*/false, fw, fh, g_video_fbo.get());
        g_renderer.Camera() = free_cam;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_video_fbo.get());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, fw, fh, fx, fy, fx + fw, fy + fh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, g_client_w, g_client_h);
    } else if (fw >= 1 && fh >= 1) {
        // No picture at the playhead (no FX, no item...): the frame is an empty surface.
        const ImVec4 empty = ui::Col(ui::kSurface);
        glEnable(GL_SCISSOR_TEST);
        glScissor(fx, fy, fw, fh);
        glClearColor(empty.x, empty.y, empty.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
}

void RenderTick()
{
    if (!g_hdc || !g_hglrc) return;

    // Re-assert OUR context every frame. This is Reaper's main thread, shared with other
    // GL-rendering plugins that make THEIR context current; ours then isn't current here
    // and every GL call silently fails — incl. LoadAsset's buffer uploads, which surfaced
    // as a systematic "gpu-upload-failed". The previous context is handed back on exit so
    // a plugin with the same "stays current" assumption isn't broken by us in turn.
    // wglGetCurrentContext is a cheap TLS read; the switch only happens when stolen.
    const HGLRC prev_rc = wglGetCurrentContext();
    const HDC   prev_dc = wglGetCurrentDC();
    const bool  switched = (prev_rc != g_hglrc);
    if (switched && !wglMakeCurrent(g_hdc, g_hglrc)) return;
    struct RestoreContext {
        bool active; HDC dc; HGLRC rc;
        ~RestoreContext() { if (active) wglMakeCurrent(dc, rc); }
    } restore{switched, prev_dc, prev_rc};

    // Story 4.3 — poll the transport for the RAV item under the playhead, SYNCHRONOUSLY
    // right before drawing (NFR-P5: no async between the playhead read and the draw,
    // architecture.md:1015). This runs on the main UI pump (the NULL-hwnd timer Reaper
    // dispatches), so the Reaper item APIs GetCurrentAnimItem calls are a correct
    // main-thread call — do NOT move this onto a worker thread.
    //
    // Story 11-4: a track pinned in the Video panel restricts the query to that track (in
    // both views, so switching views never reloads the asset); the item's track feeds the
    // panel's "follow the displayed item".
    std::string item_path;
    double item_time = 0.0;
    MediaTrack* item_track = nullptr;
    const bool item_found = GetCurrentAnimItemOn(VideoViewPinnedTrack(), item_path, item_time, &item_track);
    if (item_found) {
        if (item_path != g_current_anim_path) {
            // Item changed (first item, or a scrub onto a DIFFERENT source) → (re)load
            // its asset. This is the COLD path — gated by the cheap string compare so a
            // per-frame LoadAsset (which would blow NFR-P5 and thrash VRAM) never happens.
            // The full GPU load is safe HERE — unlike 4.2's GL-free file-drop — because
            // RenderTick runs with the viewer's WGL context current on this thread, which
            // is exactly what LoadAsset/SetAsset require (AC2).
            // Tag the loader's warnings (texture / skin lines name no file) with this path in
            // the copyable log; cleared before the load-failed line, which names it itself.
            SetLogContext(item_path);
            // The file's version BEFORE the parse (Story 11-2 cache, see OfferCpuAsset).
            const AssetFileStamp item_stamp = ReadAssetFileStamp(item_path);
            LoadResult r = LoadAsset(item_path);
            SetLogContext("");
            g_current_load_ok = r.asset.has_value();
            if (r.asset) {
                g_renderer.SetAsset(std::move(*r.asset));
                // Story 11-2: hand the parse to the video FX if one of its tracks uses this
                // file (kept read-only in the shared cache; ignored otherwise).
                OfferCpuAsset(item_path, std::move(r.cpu), item_stamp);
                LogInfo("now showing %s", item_path.c_str());
                if (r.notices.empty()) {
                    ClearLoadMessage();
                } else {
                    const bool first_time = g_noticed_paths.insert(item_path).second;
                    SetLoadMessage(false, FileNameOf(item_path) + " has problems:",
                                   std::move(r.notices), /*pop_up=*/first_time);
                }
                g_missing_tex_item  = item_path;
                g_missing_tex_files = std::move(r.missing_texture_files);
                // Issue #1: this reload follows a "Locate textures..." copy: show its report.
                if (g_locate_report_item == item_path && !g_locate_report_lines.empty()) {
                    if (g_load_msg_title.empty()) {
                        SetLoadMessage(false, FileNameOf(item_path) + ": textures found", {},
                                       /*pop_up=*/true);
                        g_load_msg_info = true;
                    }
                    g_load_msg_report = std::move(g_locate_report_lines);
                    g_load_msg_until  = ElapsedSeconds() + kLoadMessageSeconds;
                }
            } else {
                // A malformed file logs ONCE and leaves the previous asset up (AR17) —
                // never a per-frame retry storm. The literal "reload only on path change"
                // alone would re-attempt every frame while this item stays under the
                // playhead (its path never matches), so advance the gate on FAILURE too
                // (Dev Notes "Previous-story intelligence" / cold-path hardening).
                LogError("load failed [%s] %s: %s", LoadErrorCategoryName(r.category),
                         item_path.c_str(), r.detail.c_str());
                // Story 6.5.5 — rehome the load-failure signal on-canvas (AC7, amended AR16):
                // a short transient message near the top. This does NOT change the load-bearing
                // gate-advance-on-failure logic below — it only surfaces it. It shows the
                // plain-language hint; the detail is in the log its "Copy details" button copies.
                SetLoadMessage(true, "Can't load " + FileNameOf(item_path),
                               {r.hint.empty() ? std::string("This file couldn't be opened.")
                                               : r.hint},
                               /*pop_up=*/true);
                g_missing_tex_item.clear();
                g_missing_tex_files.clear();
            }
            g_locate_report_item.clear();
            g_locate_report_lines.clear();
            // Advance the gate to the current item's path whether the load succeeded or
            // failed — either way we have "handled" this path and must not retry it every
            // frame; a later scrub onto a different path re-arms the reload.
            g_current_anim_path = item_path;
        }
        g_display_time     = item_time;  // playhead-driven, already clamped to [0, itemLength]
        g_transport_driven = true;       // sticky once any RAV item has driven the view
    }
    // Not found → HOLD: leave g_display_time and g_current_anim_path unchanged so the
    // last in-span frame (already at the boundary under continuous scrubbing) freezes
    // (AC3). Do NOT revert to the looping fixture here — reload thrash + breaks "hold".

    // Story 6.5.8 — advance the ViewCube snap tween once per frame, JUST before the view is
    // derived in RenderFrame (view_ = cam_.ViewMatrix() at renderer.cpp), so the new
    // yaw/pitch shows THIS frame (smooth, no one-frame stutter). dt from QPC, clamped to a
    // sane max so a paused->resumed panel (g_last_tick reseeded to 0 on resume) or a long
    // stall can't teleport the tween a full second. No-op when no snap is in flight.
    {
        LARGE_INTEGER now_tick;
        QueryPerformanceCounter(&now_tick);
        float dt = 0.0f;
        if (g_last_tick.QuadPart != 0 && g_qpc_freq.QuadPart != 0)
            dt = static_cast<float>(double(now_tick.QuadPart - g_last_tick.QuadPart) /
                                    double(g_qpc_freq.QuadPart));
        g_last_tick = now_tick;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;  // cap a paused/stalled gap (the g_render_paused window)
        g_renderer.AdvanceCameraAnim(dt);
    }

    // Story 11-4 — tell the Video view model what is displayed (its track, whether an item
    // spans the playhead, the model's posed bounds), before the ImGui frame reads it.
    {
        const Asset& shown = g_renderer.CurrentAsset();
        const bool has_model = item_found && g_current_load_ok && !shown.meshes.empty();
        VideoViewFrame(item_found ? item_track : nullptr, item_found, has_model, shown.aabbMin, shown.aabbMax);
    }

    // Before any RAV item is ever current, g_transport_driven is false → the viewport
    // is idle (no startup fixture — the launch file-picker was removed; animations are
    // loaded from the timeline). Once an item drives the view it is sticky: transport
    // time, clamp (no loop).
    if (VideoViewActive()) {
        RenderVideoView(static_cast<float>(g_display_time));
    } else {
        g_renderer.RenderFrame(
            static_cast<float>(g_transport_driven ? g_display_time : ElapsedSeconds()),
            /*loop=*/!g_transport_driven, g_client_w, g_client_h);
    }

    // Story 11-2: the video FX renders with exactly what the viewer just drew with (light,
    // floor, grid, shadow, normal maps, MSAA, background); kept after the viewer closes.
    // A change also refreshes REAPER's cached FX frames, once the user pauses (a light
    // drag changes the settings every frame; each refresh re-renders REAPER's cache).
    {
        static double refresh_at = -1.0;
        const double now = ElapsedSeconds();
        if (PublishDisplaySettings(g_renderer.CurrentDisplaySettings())) refresh_at = now + 0.2;
        if (refresh_at >= 0.0 && now >= refresh_at) {
            refresh_at = -1.0;
            RefreshVideoFxPictures();
        }
    }

    DrawToolUi();   // Dear ImGui overlay on top of the 3D scene (Story 6.5.3 rev)

    SwapBuffers(g_hdc);

    // Story 11-4 — the Video view's REAPER writes run from the window procedure, never
    // inside this tick (a message box or a plug-in load may run a modal loop).
    if (VideoViewHasPending() && !g_video_post_pending && g_hwnd)
        g_video_post_pending = PostMessageW(g_hwnd, kMsgRunVideoCommands, 0, 0) != FALSE;

    ++g_frame_count;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double elapsed = double(now.QuadPart - g_fps_last.QuadPart) / double(g_qpc_freq.QuadPart);
    if (elapsed >= 1.0) {
        // Story 6.5.5 — STORE the rate for the on-canvas readout (the overlay reads g_fps).
        // The LogInfo stays (silent by default since 6.5.2; re-enabled only in a debug build) —
        // do NOT delete it and do NOT add any new console FPS output (FR53 replaces the log).
        g_fps = static_cast<float>(g_frame_count / elapsed);
        LogInfo("%.1f fps  (%dx%d)", g_frame_count / elapsed, g_client_w, g_client_h);
        g_frame_count = 0;
        g_fps_last    = now;
    }
}

void CALLBACK FrameTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    // Reaper sends our docked window NO message when the panel is closed/hidden
    // (proven by a Story 1.3 message trace — it just hides an ancestor). We also
    // cannot reliably tell a close apart from a transient hide (minimize, another
    // docker tab active): the states are indistinguishable from our side and the
    // timings are unpredictable, so trying to DESTROY on hide misfired and corrupted
    // the window during minimize/restore. So we don't destroy here. We just render
    // while visible and PARK while hidden — no SwapBuffers, no fps, no GPU work, so
    // nothing runs in the background. The panel is destroyed only by the toggle
    // action or unload. IsWindowVisible is true only when the whole ancestor chain
    // (the docker host) is visible too.
    if (!g_hwnd || !IsWindowVisible(g_hwnd)) {
        if (!g_render_paused) {
            g_render_paused = true;
            DropKeyClaims();
            LogInfo("panel hidden — render paused");
        }
        return;
    }
    if (g_render_paused) {
        g_render_paused = false;
        QueryPerformanceCounter(&g_fps_last);  // don't skew the first second after the pause
        g_frame_count = 0;
        g_last_tick.QuadPart = 0;  // 6.5.8 — force dt=0 next frame so a hidden gap can't jump the tween
        LogInfo("panel shown — render resumed");
    }
    RenderTick();
}

// Builds + renders the Dear ImGui tool UI for this frame, drawn on top of the 3D scene
// (called between RenderFrame and SwapBuffers). A frameless hamburger menu:
// Recenter camera (FR25), a real colour picker (FR50), and a circular light-position pad
// that orbits the light around the model (FR50). All widgets mutate state only and push
// to the renderer's light members — no synchronous re-render (AR18). NON-fatal: if ImGui
// failed to initialize, this is a no-op and the viewport still runs (AR17).
// A circular "light position" pad: the disc is the sphere around the model laid flat
// (azimuthal) — centre = light straight overhead (elevation +90°), the mid ring = the
// horizon (0°), the rim = straight below (−90°); the angle around = azimuth. Drag the
// dot to place the light in space. Returns true while the user is moving it.
bool LightDirectionPad(float diameter)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##lightpad", ImVec2(diameter, diameter));
    const bool active = ImGui::IsItemActive();
    const ImVec2 c(p0.x + diameter * 0.5f, p0.y + diameter * 0.5f);
    const float R = diameter * 0.5f - 4.0f;

    bool changed = false;
    if (active) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        float dx = m.x - c.x, dy = m.y - c.y;
        float r = std::sqrt(dx * dx + dy * dy);
        if (r > R && r > 0.0f) { dx *= R / r; dy *= R / r; r = R; }
        const float rho = (R > 0.0f) ? r / R : 0.0f;           // 0 centre … 1 rim
        g_light_azimuth   = std::atan2(dx, -dy);               // 0 = up (north)
        g_light_elevation = kHalfPi - rho * kPiF;              // +90° centre … −90° rim
        changed = true;
    }

    // Disc + horizon ring + axes.
    dl->AddCircleFilled(c, R, IM_COL32(26, 28, 34, 255), 48);
    dl->AddCircle(c, R, IM_COL32(92, 98, 112, 255), 48, 1.5f);
    dl->AddCircle(c, R * 0.5f, IM_COL32(58, 62, 72, 255), 40, 1.0f);
    dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), IM_COL32(52, 55, 64, 255));
    dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), IM_COL32(52, 55, 64, 255));

    // Handle, placed from the current azimuth/elevation.
    const float rho = (kHalfPi - g_light_elevation) / kPiF;    // 0..1
    const ImVec2 h(c.x + std::sin(g_light_azimuth) * rho * R,
                   c.y - std::cos(g_light_azimuth) * rho * R);
    dl->AddCircleFilled(h, 6.0f, IM_COL32(120, 170, 255, 255), 16);
    dl->AddCircle(h, 6.0f, IM_COL32(255, 255, 255, 255), 16, 1.5f);
    return changed;
}

// The load message's content — title (red/amber), one bullet per line, "Copy details" —
// drawn into the CURRENT ImGui window. Shared by the transient top-centre message and the
// status icon's hover panel so both always say the same thing. Text wraps so a narrow
// docked panel still shows it.
void DrawLoadMessageBody()
{
    const float wrap_w = std::max(160.0f, std::min(480.0f, g_client_w - 60.0f));
    const ImVec4 title_col = g_load_msg_error ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f)
                           : g_load_msg_info  ? ui::Col(ui::kOk)
                                              : ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
    ImGui::TextColored(title_col, "%s", g_load_msg_title.c_str());
    for (const std::string& line : g_load_msg_report) {   // issue #1: the locate outcome
        ImGui::Bullet();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kOk));
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
    }
    for (const std::string& line : g_load_msg_lines) {
        ImGui::Bullet();
        ImGui::TextUnformatted(line.c_str());
    }
    ImGui::PopTextWrapPos();
    // Issue #1: the current item has texture files not found → offer to find them. The
    // picker opens from the window procedure (kMsgLocateTextures), after this frame.
    if (!g_load_msg_error && !g_missing_tex_files.empty()) {
        if (ui::PrimaryButton("Locate textures...##locatetex") && !g_locate_pending && g_hwnd)
            g_locate_pending = PostMessageW(g_hwnd, kMsgLocateTextures, 0, 0) != FALSE;
    }
    const bool copied = ElapsedSeconds() < g_copied_until;
    if (ImGui::SmallButton(copied ? "Copied!##copyerr" : "Copy details##copyerr"))
        CopyErrorLogToClipboard();
}

// Bottom-left status icon: shown while the current item has a load status (failed, or
// loaded with problems), so the message can be read again after its transient pop-up
// is gone. Drawn with ImDrawList primitives (no icon asset): a red disc with "!" for an
// error, an amber triangle with "!" for problems. Hovering it opens the message panel
// just above it; the panel stays open while the mouse is on the icon or on the panel
// (plus a short grace to cross the gap) so its "Copy details" button can be clicked.
// Same pattern as NavCubeWidget: its own frameless window, InvisibleButton hit area.
void StatusIconWidget()
{
    if (g_load_msg_title.empty() || g_load_msg_info) return;  // info = no problem to show

    constexpr ImGuiWindowFlags kIconFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBackground;

    const float icon_top = ViewBottom() - kStatusIconMargin - kStatusIconSize;  // Story 11-5: above the strip
    ImGui::SetNextWindowPos(ImVec2(kStatusIconMargin, icon_top), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    bool icon_hovered = false;
    if (ImGui::Begin("##statusicon", nullptr, kIconFlags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##statushit", ImVec2(kStatusIconSize, kStatusIconSize));
        icon_hovered = ImGui::IsItemHovered();

        const float  s  = kStatusIconSize;
        const float  cx = p0.x + s * 0.5f;
        if (g_load_msg_error) {
            const ImU32 ink = IM_COL32(255, 255, 255, 255);
            dl->AddCircleFilled(ImVec2(cx, p0.y + s * 0.5f), s * 0.5f, IM_COL32(222, 84, 70, 255), 24);
            dl->AddRectFilled(ImVec2(cx - 1.5f, p0.y + s * 0.22f), ImVec2(cx + 1.5f, p0.y + s * 0.60f), ink, 1.0f);
            dl->AddCircleFilled(ImVec2(cx, p0.y + s * 0.75f), 1.9f, ink, 8);
        } else {
            const ImU32 ink = IM_COL32(48, 34, 8, 255);
            dl->AddTriangleFilled(ImVec2(cx, p0.y + 1.0f), ImVec2(p0.x + s - 1.0f, p0.y + s - 2.0f),
                                  ImVec2(p0.x + 1.0f, p0.y + s - 2.0f), IM_COL32(255, 191, 77, 255));
            dl->AddRectFilled(ImVec2(cx - 1.5f, p0.y + s * 0.34f), ImVec2(cx + 1.5f, p0.y + s * 0.64f), ink, 1.0f);
            dl->AddCircleFilled(ImVec2(cx, p0.y + s * 0.78f), 1.9f, ink, 8);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    const double now = ElapsedSeconds();
    if (icon_hovered) g_status_panel_until = now + kStatusPanelGrace;
    if (now >= g_status_panel_until) return;

    // The panel's bottom edge sits just above the icon (pivot bottom-left), so the mouse
    // crosses only a couple of pixels — covered by the grace — to reach "Copy details".
    constexpr ImGuiWindowFlags kPanelFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::SetNextWindowPos(ImVec2(kStatusIconMargin, icon_top - 2.0f), ImGuiCond_Always,
                            ImVec2(0.0f, 1.0f));
    if (ImGui::Begin("##statuspanel", nullptr, kPanelFlags)) {
        DrawLoadMessageBody();
        if (ImGui::IsWindowHovered()) g_status_panel_until = now + kStatusPanelGrace;
    }
    ImGui::End();
}

// Story 6.5.8 — Navigation cube (ViewCube): a small axis-coloured cube in the bottom-right
// that ROTATES in lock-step with the camera (it shows the scene's axes from the camera's
// vantage) and SNAPS the camera when a face/edge/corner is clicked. Modelled exactly on
// LightDirectionPad above — GetWindowDrawList + an InvisibleButton hit area + ImDrawList
// primitives, mutate-state-only, push to the renderer on click (AR18: no synchronous
// re-render — SnapCameraTo just arms a tween the next frames play out). The cube is a PLAIN
// SOLID cube — smooth flat axis-coloured faces, no bevel geometry. Only the HOVER HIGHLIGHT
// tells a face / edge / corner apart: a translucent fill on the face, a yellow BAR along the
// edge, or a yellow DOT on the corner (Antho's review feedback — highlight + click edges and
// corners, but don't carve faces for them). Under the hood, picking subdivides each visible
// face into a 3x3 grid (centre = face, edge cells = the shared edge, corner cells = the
// corner) only to CLASSIFY the hovered element and derive its snap direction — the cells are
// never drawn. The element yields a unit DIRECTION-from-centre; the camera derives yaw/pitch
// from it (no angle is hardcoded — face = straight-on, edge = 45, corner = ~35, all from the
// ±1 geometry). Only FRONT-facing faces are drawn and picked (back-face cull). NON-fatal:
// only ever called from DrawToolUi, which early-returns if ImGui is not ready (AR17). Lives
// in its OWN frameless ImGui window in the SAME NewFrame/Render pair.
//
// Story 11-4 — the cube shows `cam` (the free camera in RAV view, the video camera in Video
// view) and sits at the bottom-right of the view area (`right_x`, `bottom_y` = its corner;
// in Video view the frame's, over the picture). In Video view a click edits the shot under
// the playhead (queued, one undo point) instead of the free camera.
void NavCubeWidget(const OrbitCamera& cam, bool video, float right_x, float bottom_y)
{
    // A plain solid cube — 6 flat axis-coloured faces on the unit cube ([-1,1]^3). The cube
    // looks smooth; only the hover HIGHLIGHT (drawn later) distinguishes face / edge / corner.
    // Faces: X red, Y green, Z blue, the +axis brighter than the -axis so orientation reads.
    struct Face { glm::vec3 n, u, v; ImU32 col; };
    static const Face kFaces[6] = {
        { { 1, 0, 0}, {0, 1, 0}, {0, 0, 1}, IM_COL32(214,  84,  78, 255) },  // +X  red
        { {-1, 0, 0}, {0, 1, 0}, {0, 0, 1}, IM_COL32(150,  52,  48, 255) },  // -X  dark red
        { { 0, 1, 0}, {1, 0, 0}, {0, 0, 1}, IM_COL32(120, 190,  90, 255) },  // +Y  green
        { { 0,-1, 0}, {1, 0, 0}, {0, 0, 1}, IM_COL32( 80, 130,  62, 255) },  // -Y  dark green
        { { 0, 0, 1}, {1, 0, 0}, {0, 1, 0}, IM_COL32( 84, 140, 220, 255) },  // +Z  blue
        { { 0, 0,-1}, {1, 0, 0}, {0, 1, 0}, IM_COL32( 56,  96, 156, 255) },  // -Z  dark blue
    };

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBackground;  // just the cube floats over the viewport

    // Bottom-right pivot (1,1) anchored to the live client size (current in WM_SIZE) — mirrors
    // the FPS read-out's right-edge pivot but pinned to the bottom-right corner.
    ImGui::SetNextWindowPos(
        ImVec2(right_x - kNavCubeMargin, bottom_y - kNavCubeMargin),  // Story 11-5: above the shot strip
        ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (!ImGui::Begin("##navcube", nullptr, kFlags)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    // The hit area covers the whole square; IsItemHovered gates picking so the cube only
    // grabs the mouse over its own region (the camera-vs-ImGui arbitration sees WantCaptureMouse).
    const bool pressed = ImGui::InvisibleButton("##navcubehit", ImVec2(kNavCubeSize, kNavCubeSize));
    const bool hovered = ImGui::IsItemHovered();

    const ImVec2 c(p0.x + kNavCubeSize * 0.5f, p0.y + kNavCubeSize * 0.5f);
    const float scale = (kNavCubeSize * 0.5f - 6.0f) / 1.85f;  // fit the sqrt(3) cube corner inside

    // Camera view basis from the LIVE yaw/pitch — the cube is seen THROUGH the camera so it
    // turns with the view (AC1). viewDir = cube->camera (== Eye()-target, camera.h spherical
    // form). fwd = camera->cube; right/up are the screen axes. pitch is clamped to +/-1.55
    // (Orbit/snap guard) so fwd never aligns with world-up and the cross stays well-defined.
    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const float sy = std::sin(cam.yaw),   cyw = std::cos(cam.yaw);
    const glm::vec3 viewDir(cp * sy, sp, cp * cyw);
    const glm::vec3 fwd   = -viewDir;
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up    = glm::cross(right, fwd);

    auto project = [&](const glm::vec3& q) -> ImVec2 {
        return ImVec2(c.x + scale * glm::dot(q, right),   // +x right
                      c.y - scale * glm::dot(q, up));      // +y up → screen y down, so negate
    };

    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // Draw the visible faces (flat fill + thin dark outline) and, on the hovered face, classify
    // the cursor into a 3x3 cell: centre = FACE, an edge cell = the shared EDGE, a corner cell =
    // the CORNER. The cells are NEVER drawn — they only classify the pick and give the snap
    // direction normalize(n + (i-1)u + (j-1)v). Orthographic projection of a planar quad is
    // affine, so the (s,t) inverse is exact and linear (a parallelogram solve).
    int       hitFace = -1, hitI = 1, hitJ = 1;
    glm::vec3 hitDir(0.0f);
    for (int k = 0; k < 6; ++k) {
        const Face& f = kFaces[k];
        if (glm::dot(f.n, viewDir) <= 0.0f) continue;  // back-face cull — visible faces only
        const ImVec2 P00 = project(f.n - f.u - f.v), P10 = project(f.n + f.u - f.v),
                     P11 = project(f.n + f.u + f.v), P01 = project(f.n - f.u + f.v);
        const ImVec2 quad[4] = { P00, P10, P11, P01 };
        dl->AddConvexPolyFilled(quad, 4, f.col);
        dl->AddPolyline(quad, 4, IM_COL32(20, 22, 28, 220), ImDrawFlags_Closed, 1.5f);

        if (hovered && hitFace < 0) {
            const ImVec2 A(P10.x - P00.x, P10.y - P00.y);
            const ImVec2 B(P01.x - P00.x, P01.y - P00.y);
            const ImVec2 M(mouse.x - P00.x, mouse.y - P00.y);
            const float det = A.x * B.y - A.y * B.x;
            if (std::fabs(det) > 1e-5f) {
                const float s = (M.x * B.y - M.y * B.x) / det;
                const float t = (A.x * M.y - A.y * M.x) / det;
                if (s >= 0.0f && s <= 1.0f && t >= 0.0f && t <= 1.0f) {
                    int i = static_cast<int>(s * 3.0f); if (i > 2) i = 2; if (i < 0) i = 0;
                    int j = static_cast<int>(t * 3.0f); if (j > 2) j = 2; if (j < 0) j = 0;
                    hitFace = k; hitI = i; hitJ = j;
                    hitDir  = glm::normalize(f.n + float(i - 1) * f.u + float(j - 1) * f.v);
                }
            }
        }
    }

    // Highlight the hovered element ON the cube geometry (AC2) — a face fill, an edge BAR, or a
    // corner DOT — so the cube itself stays smooth. Click arms the snap (AC3): mutate-state-only,
    // RenderTick's AdvanceCameraAnim plays out the tween; no hardcoded angle.
    if (hitFace >= 0) {
        const Face& f = kFaces[hitFace];
        const bool  iEdge = (hitI != 1), jEdge = (hitJ != 1);
        const ImU32 kHi = IM_COL32(255, 213, 79, 255);   // amber highlight (matches the reference)

        if (!iEdge && !jEdge) {
            // FACE — translucent fill + outline over the whole face.
            const ImVec2 q[4] = { project(f.n - f.u - f.v), project(f.n + f.u - f.v),
                                  project(f.n + f.u + f.v), project(f.n - f.u + f.v) };
            dl->AddConvexPolyFilled(q, 4, IM_COL32(255, 213, 79, 90));
            dl->AddPolyline(q, 4, kHi, ImDrawFlags_Closed, 2.0f);
        } else if (iEdge && jEdge) {
            // CORNER — a dot at the cube corner.
            const ImVec2 cc = project(f.n + float(hitI - 1) * f.u + float(hitJ - 1) * f.v);
            dl->AddCircleFilled(cc, 7.0f, kHi, 16);
            dl->AddCircle(cc, 7.0f, IM_COL32(40, 34, 10, 255), 16, 1.5f);
        } else {
            // EDGE — a thick bar along the cube edge, rounded caps.
            glm::vec3 e0, e1;
            if (jEdge) {  // edge runs along u at v = (hitJ-1)
                e0 = f.n - f.u + float(hitJ - 1) * f.v;
                e1 = f.n + f.u + float(hitJ - 1) * f.v;
            } else {      // edge runs along v at u = (hitI-1)
                e0 = f.n + float(hitI - 1) * f.u - f.v;
                e1 = f.n + float(hitI - 1) * f.u + f.v;
            }
            const ImVec2 a = project(e0), b = project(e1);
            dl->AddLine(a, b, kHi, 6.0f);
            dl->AddCircleFilled(a, 3.0f, kHi, 12);
            dl->AddCircleFilled(b, 3.0f, kHi, 12);
        }

        if (pressed) {
            if (video) QueueVideoSnap(hitDir);
            else       g_renderer.SnapCameraTo(hitDir);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

// Builds + renders the Dear ImGui tool UI for this frame, on top of the 3D scene (called
// between RenderFrame and SwapBuffers). A FRAMELESS hamburger menu (Antho's icons) that
// show/hides a list of tools: Recenter camera (FR25), light colour (FR50), and the light
// position pad (FR50). Fixed top-left, non-resizable, looks part of the interface. All
// widgets mutate state only and push to the renderer (no synchronous re-render, AR18).
// NON-fatal: if ImGui failed to initialize this is a no-op and the viewport still runs.
void DrawToolUi()
{
    if (!g_imgui_ready) return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    // Spec 11-fb-11: open, the menu stops 10 px above the window's bottom and scrolls (a short
    // viewer with every section unfolded); closed, it is the bare button.
    constexpr float kMenuMargin = 10.0f;  // its gap to the window's top and bottom edges
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
    constexpr ImGuiWindowFlags kClosedFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(0.0f, 0.0f),
        ImVec2(FLT_MAX, std::max(48.0f, static_cast<float>(g_client_h) - 2.0f * kMenuMargin)));

    // Story 11-4: the theme (ui_theme.h) styles the menu; it is a raised card when open.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, g_menu_open ? ui::Col(ui::kRaised) : ui::Col(IM_COL32(18, 19, 23, 184)));
    ImGui::PushStyleColor(ImGuiCol_Border, g_menu_open ? ui::Col(ui::kStrokeStrong) : ui::Col(ui::kStroke));
    ImGui::Begin("##tools", nullptr, g_menu_open ? kFlags : (kFlags | kClosedFlags));

    // Hamburger button — frameless icon; toggles the option list.
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.16f));
    if (ImGui::ImageButton("##menu", IconTex(g_icon_menu), ImVec2(22.0f, 22.0f),
                           ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                           ImVec4(0.90f, 0.90f, 0.95f, 1.0f)))
        g_menu_open = !g_menu_open;
    ImGui::PopStyleColor(3);

    if (g_menu_open) {
        const ImVec4 kIconTint(0.82f, 0.84f, 0.90f, 1.0f);
        ImGui::Spacing();

        // Story 11-4 (UX decision 8): the menu is split into View and Tools groups.
        ui::Caption("VIEW");

        // A collapsible section header: Antho's icon + a CollapsingHeader (the ▸ arrow
        // collapses/expands the group). Default-open so the menu looks unchanged until the
        // user folds a section; the open/closed state is kept for the session (in-memory).
        auto Section = [&](GLuint icon, const char* label) -> bool {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::Image(IconTex(icon), ImVec2(16.0f, 16.0f),
                         ImVec2(0, 0), ImVec2(1, 1), kIconTint);
            ImGui::SameLine();
            return ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
        };

        // --- Light: Colour + Position ---
        if (Section(g_icon_light, "Light")) {
            ImGui::Indent(8.0f);
            ImGui::Image(IconTex(g_icon_color), ImVec2(17.0f, 17.0f),
                         ImVec2(0, 0), ImVec2(1, 1), kIconTint);
            ImGui::SameLine();
            if (ImGui::ColorEdit3("##colour", g_light_color,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha))
                g_renderer.SetLightColor(glm::vec3(g_light_color[0], g_light_color[1], g_light_color[2]));
            ImGui::SameLine();
            ImGui::TextUnformatted("Colour");

            ImGui::Dummy(ImVec2(0.0f, 3.0f));
            ImGui::TextUnformatted("Position");
            if (LightDirectionPad(150.0f))
                g_renderer.SetLightDir(LightDirFromAngles(g_light_azimuth, g_light_elevation));

            // Lighting-quality knobs to fix the "flat/cheap vs Mixamo" look in-Reaper, live:
            // Ambient = fill (lower → more contrast), Specular = sheen, Relief = normal-map boost.
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::SliderFloat("Ambient",  &g_ambient,        0.0f, 1.0f, "%.2f"))
                g_renderer.SetAmbient(g_ambient);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::SliderFloat("Specular", &g_spec_strength,  0.0f, 2.0f, "%.2f"))
                g_renderer.SetSpecStrength(g_spec_strength);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::SliderFloat("Relief",   &g_normal_strength, 0.0f, 3.0f, "%.2f"))
                g_renderer.SetNormalStrength(g_normal_strength);
            ImGui::Unindent(8.0f);
        }

        // --- Ground: show/hide the floor + grid (hiding it also drops the cast shadow,
        // the floor being the only receiver) ---
        if (Section(g_icon_ground, "Ground")) {
            ImGui::Indent(8.0f);
            if (ImGui::Checkbox("Enable", &g_floor_visible))
                g_renderer.SetFloorVisible(g_floor_visible);
            // Epic 9 — grid cell size in real metres; the renderer converts it to the
            // file's unit, so sizes read in cells whatever the format.
            // Exclusive choices are segmented controls (house style): the picked one stands out.
            ImGui::TextUnformatted("Grid");
            ImGui::SameLine();
            static const char* const kGridLabels[2] = { "1 m", "10 m" };
            int grid_sel = (g_grid_step_m == 10) ? 1 : 0;
            if (ui::Segmented("##grid", kGridLabels, 2, &grid_sel, 0.0f, -1)) {
                g_grid_step_m = (grid_sel == 1) ? 10 : 1;
                g_renderer.SetGridStep(static_cast<float>(g_grid_step_m));
            }
            ImGui::Unindent(8.0f);
        }

        // --- Shadow: cast-shadow quality (Off skips the depth pass; no floor → no shadow) ---
        if (Section(g_icon_shadow, "Shadow")) {
            ImGui::Indent(8.0f);
            static const char* const kShadowLabels[4] = { "Off", "Low", "Mid", "High" };
            if (ui::Segmented("##shadow", kShadowLabels, 4, &g_shadow_quality, 0.0f, -1))
                g_renderer.SetShadowQuality(static_cast<ShadowQuality>(g_shadow_quality));
            ImGui::Unindent(8.0f);
        }

        // --- Performance: render-quality levers (FR52) + on-canvas FPS (FR53). Antho's
        // Performance icon. The Ground (floor) + Shadow levers live in their own sections above
        // and TOGETHER with these complete the FR52 set — not duplicated here. ---
        if (Section(g_icon_performance, "Performance")) {
            ImGui::Indent(8.0f);

            if (ImGui::Checkbox("Normal maps", &g_normal_maps_on))
                g_renderer.SetNormalMapsEnabled(g_normal_maps_on);

            // Story 6.5.6 — MSAA level selector (FR52), replacing 6.5.5's on/off checkbox: a row
            // of Off / 2× / 4× / 8× radio buttons, mirroring the Shadow selector. An option whose
            // sample count exceeds the GPU's GL_MAX_SAMPLES is shown disabled (Off + any level
            // <= max stay enabled) — a 2015-era iGPU often caps at 8, some at 4. On a change, push
            // the level to the renderer; it (re)allocates the offscreen MS targets on the next
            // frame (cold path) and resolves them to the window, so the change takes effect live.
            // ("2x"/"4x"/"8x" use an ASCII 'x', not '×': the build has no /utf-8 and a raw UTF-8
            // literal trips MSVC C4566 under /W3 — same reason the 6.5.3 labels stay ASCII. The
            // "Off##msaa" id-tag keeps this radio distinct from the Shadow section's "Off".)
            ImGui::TextUnformatted("MSAA");
            // Segmented (house style). Segmented greys out ONE option: the first level the GPU
            // cannot do; a pick above it (possible when two levels exceed the cap) is ignored.
            ImGui::SameLine();
            static const int         kMsaaLevels[4] = { 0, 2, 4, 8 };
            static const char* const kMsaaLabels[4] = { "Off", "2x", "4x", "8x" };
            int msaa_sel = 0;
            int msaa_first_unavailable = -1;
            for (int i = 0; i < 4; ++i) {
                if (kMsaaLevels[i] == g_msaa_level) msaa_sel = i;
                if (msaa_first_unavailable < 0 && kMsaaLevels[i] > g_msaa_max) msaa_first_unavailable = i;
            }
            if (ui::Segmented("##msaa", kMsaaLabels, 4, &msaa_sel, 0.0f, msaa_first_unavailable) &&
                kMsaaLevels[msaa_sel] <= g_msaa_max) {
                g_msaa_level = kMsaaLevels[msaa_sel];
                g_renderer.SetMsaaSamples(g_msaa_level);
            }

            ImGui::Checkbox("FPS", &g_fps_overlay_on);  // pure UI state — read by the overlay below
            // How long REAPER's Video window takes to catch up with an output size or display
            // change while playing, on Video view's caption line (spec 11-fb-5).
            ImGui::Checkbox("REAPER catch-up", &g_preview_lag_on);

            ImGui::Unindent(8.0f);
        }

        // --- Tools: Video shows / hides the Video panel (Story 11-4, UX decision 8) ---
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::Separator();
        ui::Caption("TOOLS");
        {
            const bool panel = VideoPanelVisible();
            if (ImGui::Selectable("Video##toolsvideo", panel, 0, ImVec2(196.0f, 0.0f)))
                SetVideoPanelVisible(!panel);
            ImGui::SameLine(160.0f);
            ui::Caption(panel ? "open" : "closed");
            // Spec 11-fb-11 -- the Delete key asks first unless "Don't ask again" was ticked;
            // this gives the question back (or takes it away).
            ImGui::Indent(8.0f);
            bool ask = VideoAskBeforeDeleteShot();
            if (ImGui::Checkbox("Ask before deleting a shot", &ask)) SetVideoAskBeforeDeleteShot(ask);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("The %s key in Video view deletes the current shot: ask first, or delete at once",
                                  ShortcutKeyLabel(kShortcutDeleteShot));
            ImGui::Unindent(8.0f);
        }

        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::Separator();
        if (ImGui::Button("Recenter camera", ImVec2(196.0f, 0.0f)))
            g_renderer.ResetCamera();

        // Always enabled: even with no error kept, the GPU line helps a bug report.
        const bool copied = ElapsedSeconds() < g_copied_until;
        if (ImGui::Button(copied ? "Copied!##copylog" : "Copy error log##copylog",
                          ImVec2(196.0f, 0.0f)))
            CopyErrorLogToClipboard();

        // Build version (a dev build shows "<last release>-dev+<commit>"), so a user can tell
        // which one they run.
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::TextDisabled("Version %s", RAV_DISPLAY_VERSION);
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    // Story 6.5.5 — extra frameless read-out overlays, drawn in the SAME NewFrame/Render pair
    // (never a second NewFrame). NoInputs so they can NEVER steal the mouse from the camera
    // (the camera-vs-ImGui arbitration is ImGuiWantsMouse()/WantCaptureMouse — a readout that
    // grabbed focus would block orbit/zoom). Both are pure read-outs of session state.
    constexpr ImGuiWindowFlags kReadoutFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoInputs;

    // Story 11-4 — the view area: the window minus the Video panel's column when it shows.
    const bool  video_view = VideoViewActive();
    const float panel_room = VideoPanelFootprint(g_client_w);
    const float area_right = static_cast<float>(g_client_w) - panel_room;

    // FPS readout, top-RIGHT (FR53), anchored with a right-edge pivot so it hugs the corner at
    // any client size. This REPLACES the console FPS log removed in 6.5.2 — no console output.
    // Story 11-4: at the view area's corner, left of the panel button in Video view.
    const float corner_x = area_right - (video_view ? 46.0f : 10.0f);
    if (g_fps_overlay_on) {
        ImGui::SetNextWindowPos(ImVec2(corner_x, 14.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kMuted));
        if (ImGui::Begin("##fps", nullptr, kReadoutFlags))
            ImGui::Text("%.0f FPS", g_fps);
        ImGui::End();
        ImGui::PopStyleColor();
    }

    // Spec 11-fb-3 — the keyboard icon (Shortcuts popup), left of the FPS readout, centred on
    // its text line; at the corner when the readout is hidden. A fixed slot ("000 FPS") so the
    // icon does not jitter as the digits change.
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float text_mid = 14.0f + st.WindowPadding.y + ImGui::GetTextLineHeight() * 0.5f;
        const float fps_w = g_fps_overlay_on
            ? ImGui::CalcTextSize("000 FPS").x + 2.0f * st.WindowPadding.x + 2.0f : 0.0f;
        DrawShortcutsButton(corner_x - fps_w, text_mid, IconTex(g_icon_keyboard));
    }

    // Story 11-4 — RAV view / Video view toggle (top centre of the view area, key V); in
    // Video view the frame's edge + captions, its state message, and the panel button.
    DrawVideoViewToggle(area_right * 0.5f);
    if (video_view) {
        DrawVideoFrameDecor(g_video_frame, &CopyErrorLogToClipboard, g_preview_lag_on);
        DrawVideoPanelButton(area_right - 8.0f);
        // Story 11-5 -- the shot strip under the viewport (UX decision 4).
        if (VideoStripBand() > 0.0f) {
            const float strip_h = VideoStripHeight(static_cast<float>(g_client_h));
            DrawVideoShotStrip(kVideoStripGap, static_cast<float>(g_client_h) - kVideoStripGap - strip_h,
                               std::max(1.0f, area_right - 2.0f * kVideoStripGap), strip_h);
        }
    }
    if (panel_room > 0.0f) {
        const float gap = 8.0f;
        DrawVideoPanel(area_right + gap, gap, panel_room - 2.0f * gap,
                       std::max(1.0f, static_cast<float>(g_client_h) - 2.0f * gap), g_renderer.Camera(),
                       &CopyErrorLogToClipboard);
    }
    // Spec 11-fb-11 -- the Delete key's confirmation (closes itself outside Video view).
    DrawVideoDeleteConfirm();

    // Load message (AC7) — a brief transient message, top-centre, shown while fresh
    // (kLoadMessageSeconds, armed by SetLoadMessage). Rehomes the OTHER 6.5.2-silenced
    // signal on-canvas (amended AR16); no popup, no block — it just disappears when it
    // expires. Red title = the file could not load; amber = loaded, with problems. Text wraps
    // so a narrow docked panel still shows it. Unlike the FPS read-out it takes input, for its
    // "Copy details" button: it only captures the mouse while hovered (its own rect), and
    // hovering holds it.
    if (!g_load_msg_title.empty() && ElapsedSeconds() < g_load_msg_until) {
        // Story 11-4: under the view toggle (which owns the top centre now).
        // Spec 11-fb-8: in Video view, below the caption band (not over the captions).
        ImGui::SetNextWindowPos(ImVec2(area_right * 0.5f, video_view ? VideoTopBand() + 4.0f : 48.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        if (ImGui::Begin("##loadmsg", nullptr, kReadoutFlags & ~ImGuiWindowFlags_NoInputs)) {
            DrawLoadMessageBody();
            if (ImGui::IsWindowHovered() && g_load_msg_until < ElapsedSeconds() + 1.0)
                g_load_msg_until = ElapsedSeconds() + 1.0;
        }
        ImGui::End();
    }

    // Story 6.5.8 — navigation cube (ViewCube), bottom-right. Drawn in THIS NewFrame/Render
    // pair (after the menu + read-outs, before Render) so it is the same single ImGui frame.
    // Story 11-4: in Video view it shows and edits the video camera, and is hidden when there
    // is no camera to edit (no active FX / no item).
    if (!video_view) {
        NavCubeWidget(g_renderer.Camera(), /*video=*/false, area_right, ViewBottom());
    } else {
        // Over the picture's bottom-right corner: the frame keeps no room for it.
        OrbitCamera video_cam;
        if (VideoViewCamera(&video_cam))
            NavCubeWidget(video_cam, /*video=*/true, g_video_frame.x + g_video_frame.w, g_video_frame.y + g_video_frame.h);
    }

    // Bottom-left status icon + its hover panel (re-shows the load message on demand).
    StatusIconWidget();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// Brings up the GL context + renderer + frame loop. Returns false (with a
// console diagnostic) on any failure so WM_CREATE can bail without a broken
// window. (Story 1.4 hardens this into the full symmetric teardown-on-bail path.)
bool StartRendering(HWND hwnd)
{
    if (!CreateGLContextFor(hwnd)) {
        LogError("failed to create an OpenGL context — your GPU/driver may not support "
                 "OpenGL or no display is available; the viewer did not open (Reaper is unaffected)");
        return false;
    }
    if (!wglMakeCurrent(g_hdc, g_hglrc)) {
        LogError("failed to activate the OpenGL context (wglMakeCurrent); the viewer "
                 "did not open (Reaper is unaffected)");
        return false;
    }

    // The context is current here, so the GL 1.1 query entry points (exported
    // directly by opengl32 — no wglGetProcAddress loader needed) are callable.
    // Probe the driver's reported version/renderer NOW so that if the modern-GL
    // load below fails — the common "driver too old" case — we can name what the
    // driver actually reports in a human-readable remedy instead of only the
    // terse internal symbol name. glGetString may return null; guard it.
    const char* gl_version  = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* gl_renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (!gl_version)  gl_version  = "unknown";
    if (!gl_renderer) gl_renderer = "unknown";
    g_gl_version  = gl_version;   // kept for the "Copy error log" header
    g_gl_renderer = gl_renderer;

    // Story 6.5.6 — query the GPU's max MSAA sample count once, now the context is current
    // (glGetIntegerv is GL 1.1, GL_MAX_SAMPLES just an enum — no loader needed yet). It clamps
    // both the level selector (an option above it is shown disabled) AND the requested level
    // here, so we never pass glRenderbufferStorageMultisample a count it would reject with
    // GL_INVALID_VALUE. Then seed+clamp g_msaa_level and push it to the renderer so UI and
    // render agree from frame 1 (mirrors the g_floor_visible seed). A GPU reporting less than
    // the default level drops to the largest valid level (or Off). GL 3.0 guarantees >= 4.
    glGetIntegerv(GL_MAX_SAMPLES, &g_msaa_max);
    if (g_msaa_max < 0) g_msaa_max = 0;
    if (g_msaa_level > g_msaa_max)
        g_msaa_level = (g_msaa_max >= 4) ? 4 : (g_msaa_max >= 2 ? 2 : 0);
    g_renderer.SetMsaaSamples(g_msaa_level);
    LogInfo("MSAA: GL_MAX_SAMPLES=%d, level=%dx", g_msaa_max, g_msaa_level);

    std::string err;
    if (!LoadGlFunctions(err)) {
        // Lead with the cause the user can act on (update the driver), naming the
        // actual reported version/renderer; keep the precise symbol-level line too
        // (it is the exact diagnostic for log-mining).
        LogError("OpenGL 3.3+ required but your driver reports \"%s\" on \"%s\" — update "
                 "your GPU driver; the viewer did not open (Reaper is unaffected)",
                 gl_version, gl_renderer);
        LogError("%s", err.c_str());
        return false;
    }
    if (!g_renderer.Init(err)) {
        LogError("the 3D renderer failed to initialize: %s; the viewer did not open "
                 "(Reaper is unaffected)", err.c_str());
        return false;
    }

    // No startup asset: the viewport opens IDLE and stays blank until a RAV animation
    // item passes under the playhead, at which point RenderTick lazily loads and shows
    // it (Story 4.3+). The timeline is now the only load path — the legacy "choose a 3D
    // model" launch dialog was removed (animations are placed as items; an in-Reaper
    // file browser/preview is Epic 5). Until an item drives the view, g_transport_driven
    // is false and RenderFrame just clears to the idle background (no asset = no draw,
    // the same already-proven path the old "cancel" case took).
    LogInfo("viewport idle — drop an animation on a track and move the playhead over it");

    TrySetVsync(0);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    g_client_w = (rc.right  > rc.left) ? (rc.right  - rc.left) : 1;
    g_client_h = (rc.bottom > rc.top)  ? (rc.bottom - rc.top)  : 1;

    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_qpc_start);
    g_fps_last    = g_qpc_start;
    g_frame_count = 0;
    g_render_paused = false;

    g_timer_id = SetTimer(nullptr, 0, kFrameTimerMs, &FrameTimerProc);
    if (!g_timer_id) {
        LogError("failed to start the viewer's render timer (SetTimer); the viewer "
                 "did not open (Reaper is unaffected)");
        return false;
    }

    // Story 6.5.3 (rev): bring up Dear ImGui for the in-viewport tool UI. The GL
    // context is current here (required by ImGui_ImplOpenGL3_Init). NON-fatal: a failed
    // ImGui init just means no tool UI — the viewport still renders (AR17). Seed the UI
    // light state from the renderer's current direction/colour so the pad/picker
    // start in sync. ImGui input arrives via ImGui_ImplWin32_WndProcHandler in WindowProc.
    IMGUI_CHECKVERSION();
    if (ImGui::CreateContext()) {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // do not write imgui.ini next to Reaper
        io.LogFilename = nullptr;
        ui::ApplyRavTheme();   // Story 11-4: the DM-XYZ-Pad theme (ui_theme.h), over StyleColorsDark
        // Init the two backends in nested steps so teardown stays symmetric: if the GL
        // backend fails AFTER the Win32 backend already initialized, we must still shut the
        // Win32 backend down (DestroyContext alone leaks its platform data). [Review][Patch]
        if (ImGui_ImplWin32_Init(hwnd)) {
            if (ImGui_ImplOpenGL3_Init("#version 130")) {
                g_imgui_ready = true;
                const glm::vec3 d = glm::normalize(g_renderer.LightDir());
                g_light_elevation = std::asin(d.y < -1.0f ? -1.0f : (d.y > 1.0f ? 1.0f : d.y));
                g_light_azimuth   = std::atan2(d.x, d.z);
                const glm::vec3 c = g_renderer.LightColor();
                g_light_color[0] = c.r; g_light_color[1] = c.g; g_light_color[2] = c.b;
                g_ambient         = g_renderer.Ambient();
                g_spec_strength   = g_renderer.SpecStrength();
                g_normal_strength = g_renderer.NormalStrength();
                // Story 6.5.5 — the Normal-maps toggle starts at its UI default (on, quality);
                // push it so UI and render agree from frame 1 (mirrors the g_floor_visible
                // default-on contract). The MSAA level (6.5.6) is seeded+clamped earlier in
                // StartRendering, once GL_MAX_SAMPLES is known.
                g_renderer.SetNormalMapsEnabled(g_normal_maps_on);
                // Upload Antho's icons (context is current). A 0 handle just means no glyph.
                g_icon_menu   = UploadIconTexture(kIcon_menu,   kIconSize, kIconSize);
                g_icon_light  = UploadIconTexture(kIcon_light,  kIconSize, kIconSize);
                g_icon_color  = UploadIconTexture(kIcon_color,  kIconSize, kIconSize);
                g_icon_shadow = UploadIconTexture(kIcon_shadow, kIconSize, kIconSize);
                g_icon_ground = UploadIconTexture(kIcon_ground, kIconSize, kIconSize);
                g_icon_performance = UploadIconTexture(kIcon_performance, kIconSize, kIconSize);
                g_icon_keyboard = UploadIconTexture(kIcon_keyboard, kIconSize, kIconSize);
            } else {
                LogInfo("tool UI (Dear ImGui) could not initialize — the viewport still works");
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
            }
        } else {
            LogInfo("tool UI (Dear ImGui) could not initialize — the viewport still works");
            ImGui::DestroyContext();
        }
    }

    // Paint one frame now (the context is current) so the window presents real
    // content the instant the docker reveals it, instead of a ~1-tick flash of
    // the still-undefined GL framebuffer.
    RenderTick();
    return true;
}

// Symmetric teardown: kill the timer, then release GL objects while the context
// is still current, then drop the context. Order matters — GL deletes need a
// current context. Idempotent (safe even if StartRendering bailed mid-way).
void StopRendering()
{
    if (g_timer_id) {
        KillTimer(nullptr, g_timer_id);
        g_timer_id = 0;
    }
    if (g_hglrc) {
        wglMakeCurrent(g_hdc, g_hglrc);
        // ImGui's GL objects (and our icon textures) must be freed while the context is
        // current, before the renderer's and before the context itself is dropped.
        if (g_icon_menu)   { glDeleteTextures(1, &g_icon_menu);   g_icon_menu   = 0; }
        if (g_icon_light)  { glDeleteTextures(1, &g_icon_light);  g_icon_light  = 0; }
        if (g_icon_color)  { glDeleteTextures(1, &g_icon_color);  g_icon_color  = 0; }
        if (g_icon_shadow) { glDeleteTextures(1, &g_icon_shadow); g_icon_shadow = 0; }
        if (g_icon_ground) { glDeleteTextures(1, &g_icon_ground); g_icon_ground = 0; }
        if (g_icon_performance) { glDeleteTextures(1, &g_icon_performance); g_icon_performance = 0; }
        if (g_icon_keyboard) { glDeleteTextures(1, &g_icon_keyboard); g_icon_keyboard = 0; }
        if (g_imgui_ready) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            g_imgui_ready = false;
        }
        ReleaseVideoTarget();  // Story 11-4: the Video view's target, while the context is current
        g_renderer.Shutdown();
    }
    DestroyGLContext();

    // Story 11-4: a gesture or queued write cannot outlive the panel (nothing is written).
    VideoViewOnViewerClosed();
    g_drag_video = false;
    g_video_post_pending = false;
    g_current_load_ok = false;

    // The renderer's asset died with the context, so forget which item it showed: a
    // reopened panel must reload the item under the playhead, not skip it as "already
    // loaded" (the reload gate compares against this path) and draw nothing.
    g_current_anim_path.clear();
    g_transport_driven = false;
    g_noticed_paths.clear();   // a reopened panel pops each file's notices up again
    ClearLoadMessage();        // the status belongs to the item, reloaded on reopen
    g_missing_tex_item.clear();
    g_missing_tex_files.clear();
    g_locate_report_item.clear();
    g_locate_report_lines.clear();
    g_locate_pending = false;  // a post not yet dispatched must not disable the button
}

// True when the cursor is over an ImGui window/widget, so the camera must ignore the
// drag/wheel (ImGui gets it instead). Safe before ImGui init (guarded by g_imgui_ready).
bool ImGuiWantsMouse()
{
    return g_imgui_ready && ImGui::GetIO().WantCaptureMouse;
}

// Spec 11-fb-17 -- the viewer's key state right now: a text field active, a key being recorded.
bool TextInputNow() { return g_imgui_ready && ImGui::GetIO().WantTextInput; }
bool ViewerTakesCharNow() { return ViewerTakesChar(g_key_route, ShortcutRecordingId() >= 0, TextInputNow()); }

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Feed input to Dear ImGui first (Story 6.5.3 rev). Only after it is initialized —
    // StartRendering (which inits ImGui) runs synchronously inside WM_CREATE, so earlier
    // messages must not reach the handler. If ImGui fully handles a message, stop here.
    // Spec 11-fb-12: the backend reads the modifiers only on key messages, and a modifier's own
    // key down / up may have gone to REAPER or another window: refresh them before every mouse
    // message so ImGui sees Alt + wheel, and a modifier released elsewhere is not left down.
    if (g_imgui_ready && (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL || msg == WM_MOUSEMOVE ||
                          (msg >= WM_LBUTTONDOWN && msg <= WM_MBUTTONDBLCLK) ||
                          msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP || msg == WM_XBUTTONDBLCLK)) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, GetKeyState(VK_CONTROL) < 0);
        io.AddKeyEvent(ImGuiMod_Shift, GetKeyState(VK_SHIFT) < 0);
        io.AddKeyEvent(ImGuiMod_Alt, GetKeyState(VK_MENU) < 0);
    }
    if (g_imgui_ready && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;

    switch (msg) {
    case WM_CREATE:
        // AR18 — the no-throw boundary must hold HERE too, not only at
        // ToggleViewerWindow: StartRendering runs synchronously inside
        // CreateWindowExW (a USER32 callback), so an exception thrown here (e.g.
        // std::bad_alloc from the err string or g_renderer.Init) would have to
        // unwind across the Win32 frame — UB / std::terminate under /EHsc, i.e. a
        // host crash the outer try/catch can NOT catch across that boundary. Catch
        // it right here and convert to a clean -1 abort so nothing escapes USER32.
        try {
            if (!StartRendering(hwnd)) {
                // Tear down whatever partially came up, explicitly: Windows does NOT
                // send WM_DESTROY when WM_CREATE returns -1 (only WM_NCDESTROY), so we
                // cannot rely on the WM_DESTROY teardown here. StopRendering() is
                // idempotent, so even if a WM_DESTROY *did* arrive the second teardown
                // is a harmless no-op — don't "simplify" this explicit call away.
                StopRendering();   // no broken/half-initialized window is left behind
                return -1;         // abort window creation
            }
        } catch (const std::exception& e) {
            LogError("viewer init failed: %s (Reaper is unaffected)", e.what());
            StopRendering();
            return -1;
        } catch (...) {
            LogError("viewer init failed: unknown error (Reaper is unaffected)");
            StopRendering();
            return -1;
        }
        return 0;

    case WM_PAINT: {
        // The timer renders; paint just validates the update region.
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        // Store the new client size; the next timer frame recomputes viewport +
        // projection from it (no context/buffer recreation on resize). Covers the
        // docker resizing us and undock/redock (Reaper reparents the same HWND;
        // CS_OWNDC keeps the WGL DC valid across the reparent).
        g_client_w = (LOWORD(lp) > 0) ? LOWORD(lp) : 1;
        g_client_h = (HIWORD(lp) > 0) ? HIWORD(lp) : 1;
        // The tool menu is drawn from the renderer at the live client size every frame,
        // so there is nothing to re-lay-out on resize (the menu is anchored top-left).
        return 0;

    case WM_ERASEBKGND:
        return 1;  // GL owns the surface; skip GDI background fill to avoid flicker.

    case WM_LBUTTONDOWN:
        // Left click is ImGui's (widgets); the camera itself uses right/middle. Take focus
        // either way, so the wheel keeps targeting the viewport and (Story 11-4) the keys V /
        // P and a text field of the Video panel get the keyboard.
        SetFocus(hwnd);
        return 0;

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        // Start an orbit (right) or pan (middle) drag — unless the cursor is over the ImGui
        // UI. SetFocus so WM_MOUSEWHEEL (delivered to the focused window, not the hovered
        // one, §D) reaches us; SetCapture so the drag keeps reporting moves even if the
        // cursor leaves the window. Story 11-4: in Video view the drag edits the shot under
        // the playhead (never the free camera); nothing to edit = no drag.
        if (ImGuiWantsMouse()) return 0;
        // A second button during a Video view drag is ignored: it would end the video
        // gesture unwritten and move the hidden free camera.
        if (g_drag_video) return 0;
        SetFocus(hwnd);
        const bool pan = (msg == WM_MBUTTONDOWN);
        g_drag_video = VideoViewActive();
        if (g_drag_video && !VideoViewBeginDrag(pan)) {
            g_drag_video = false;
            return 0;
        }
        SetCapture(hwnd);
        g_drag   = pan ? DragMode::Pan : DragMode::Orbit;
        g_last_x = GET_X_LPARAM(lp);
        g_last_y = GET_Y_LPARAM(lp);
        return 0;
    }

    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (g_drag != DragMode::None) {
            // Story 11-4: the end of a Video view drag writes the shot (one undo point).
            if (g_drag_video) VideoViewEndDrag();
            g_drag_video = false;
            g_drag = DragMode::None;
            ReleaseCapture();
        }
        // Return 0 WITHOUT forwarding to DefWindowProc: a forwarded WM_RBUTTONUP
        // would synthesize WM_CONTEXTMENU (a Win32 popup menu) inside the panel (AC6).
        return 0;

    case WM_MOUSEMOVE:
        if (g_drag != DragMode::None) {
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            const int dx = x - g_last_x;
            const int dy = y - g_last_y;
            // Mutate camera state only; never render synchronously here — the ~64 Hz
            // timer redraws, which is what keeps manipulation continuous (FR26).
            if (g_drag_video)                   VideoViewDrag(dx, dy);  // Story 11-4
            else if (g_drag == DragMode::Orbit) g_renderer.Camera().Orbit(dx, dy);
            else                                g_renderer.Camera().Pan(dx, dy);
            g_last_x = x;
            g_last_y = y;
        }
        return 0;

    case WM_MOUSEWHEEL: {
        // Let ImGui have the wheel when the cursor is over its UI (e.g. scrolling a
        // slider's value); otherwise it zooms the camera. Spec 11-fb-12: Alt + wheel there
        // (the shot strip's zoom) also keeps Alt's release from REAPER's menu bar.
        if (ImGuiWantsMouse()) {
            if (GetKeyState(VK_MENU) < 0) g_key_route.alt_wheel_taken = true;
            return 0;
        }
        const float delta = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;  // 120 == one notch
        // Story 11-4: in Video view the wheel zooms the shot under the playhead (notches
        // close together are one gesture, one undo point).
        if (VideoViewActive()) VideoViewWheel(delta);
        else                   g_renderer.Camera().Zoom(delta);
        return 0;
    }

    case WM_CAPTURECHANGED:
        // The system can yank capture away (e.g. another window grabs it); clear the
        // drag so a later move doesn't orbit/pan with no button held. Story 11-4: a Video
        // view drag still writes what it showed (its release is lost, not the gesture).
        // Not when WE are the new owner: ImGui's backend captures on the button-down, then
        // our SetCapture re-captures and Windows still sends this. Ending the drag there
        // killed every Video view orbit/pan at its first instant (found 2026-10-03).
        if (reinterpret_cast<HWND>(lp) == hwnd) return 0;
        if (g_drag_video) VideoViewEndDrag();
        g_drag_video = false;
        g_drag = DragMode::None;
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        // Story 11-4 / spec 11-fb-3 — the viewer's keys (src/shortcuts.h: V toggles RAV view /
        // Video view, P shows / hides the Video panel, C cuts; all rebindable, Alt ones come as
        // WM_SYSKEYDOWN). The accelerator hook (ViewerTranslateAccel) sends us only those keys,
        // plus Esc while the Shortcuts popup is open, plus every key while an ImGui text field
        // is active (then they are the field's, not ours) or while a new key is recorded.
        const bool ctrl  = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        const bool alt   = GetKeyState(VK_MENU) < 0;
        if (ShortcutRecordingId() >= 0) {
            ShortcutRecordKey(static_cast<unsigned>(wp), ctrl, shift, alt);
            return 0;
        }
        if (wp == VK_ESCAPE && ShortcutsPopupOpen()) {
            RequestCloseShortcutsPopup();
            return 0;
        }
        if (wp == VK_ESCAPE && VideoDeleteConfirmOpen()) {  // spec 11-fb-11: Esc cancels the delete
            CancelVideoDelete();
            return 0;
        }
        if (wp == VK_RETURN && VideoDeleteConfirmOpen()) {  // ...and Enter confirms it (the dialog's choice)
            if (!(lp & (1 << 30))) RequestVideoDeleteConfirmByKey();
            return 0;
        }
        if (g_imgui_ready && ImGui::GetIO().WantTextInput) return 0;
        const int action = ShortcutActionForKey(static_cast<unsigned>(wp), ctrl, shift, alt, VideoViewActive());
        if (action < 0) {
            // The auto-repeat of a key the hook took (it may map to no action now): ours.
            if ((lp & (1 << 30)) && g_key_route.claimed_vk != 0 && wp == g_key_route.claimed_vk) return 0;
            break;  // not ours (an Alt key goes on to DefWindowProc)
        }
        if (lp & (1 << 30)) return 0;  // auto-repeat of a held key: one toggle per press
        if (VideoDeleteConfirmOpen()) return 0;  // spec 11-fb-11: no shortcut runs behind the confirmation
        if (action == kShortcutToggleView || (action == kShortcutTogglePanel && !VideoViewActive())) {
            // A running drag ends before the view changes (a Video view drag is written).
            // The panel key from RAV view opens the panel, which enters Video view: same rule.
            if (g_drag != DragMode::None) {
                if (g_drag_video) VideoViewEndDrag();
                g_drag_video = false;
                g_drag = DragMode::None;
                ReleaseCapture();
            }
            if (action == kShortcutToggleView) SetVideoViewActive(!VideoViewActive());
            else                               SetVideoPanelVisible(true);
            return 0;
        }
        if (action == kShortcutTogglePanel) { SetVideoPanelVisible(!VideoPanelVisible()); return 0; }
        // Story 11-5 -- cut a new shot at the playhead (Video view only: the table's context).
        if (action == kShortcutCut) { QueueVideoCut(); return 0; }
        // Spec 11-fb-11 -- delete the current shot (Video view only), after a confirmation.
        if (action == kShortcutDeleteShot) { RequestVideoDeleteCurrentShot(); return 0; }
        // The shot camera clipboard, on the current shot (Video view only).
        if (action == kShortcutCopyCamera) { QueueVideoCopyShotCamera(-1); return 0; }
        if (action == kShortcutPasteCamera) { QueueVideoPasteShotCamera(-1); return 0; }
        return 0;
    }

    case WM_GETDLGCODE: {
        // Spec 11-fb-17 — the Windows beep on the viewer's keys. We are a child of REAPER's
        // docker, a dialog, and REAPER passes each message through its IsDialogMessage. A
        // child that answers 0 here does not want characters, so IsDialogMessage took a
        // WM_CHAR ("c" after C cut, Ctrl+A's 0x01 in a text field) as a mnemonic, found no
        // control for it and beeped. Characters are always claimed (no mnemonic beep for any
        // of them). Enter / Esc / Tab / the arrows are claimed only while the viewer takes
        // keys (a text field, recording, the Shortcuts popup or the delete confirmation, a
        // key it took); otherwise the docker keeps its own handling of them. Keys the viewer
        // does not take are still returned to REAPER (0) by the accelerator hook.
        const bool takes_keys = TextInputNow() || ShortcutRecordingId() >= 0 || ShortcutsPopupOpen() ||
                                VideoDeleteConfirmOpen() || g_key_route.claimed_vk != 0;
        return DLGC_WANTCHARS | (takes_keys ? (DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTTAB) : 0);
    }

    case WM_CHAR:
    case WM_DEADCHAR:
    case WM_SYSCHAR:
    case WM_SYSDEADCHAR:
        // Spec 11-fb-17 — the character of a key we took, or one typed into a text field /
        // while recording: ImGui already has it (its handler ran above), so it ends here,
        // never in DefWindowProc (no beep, no window menu for an Alt one). Other characters
        // keep their old path.
        if (ViewerTakesCharNow()) return 0;
        break;

    case WM_SYSKEYUP:
        // Spec 11-fb-3 — an Alt key the viewer took (or recorded) must not reach the window
        // menu through DefWindowProc. The hook clears the claim on the release before it
        // reaches us, so the release is recognised by its own marker.
        if (g_key_route.released_vk != 0 && wp == g_key_route.released_vk) {
            g_key_route.released_vk = 0;
            return 0;
        }
        // Spec 11-fb-17 — Alt released in a text field (or while recording) must not open
        // REAPER's menu bar (SC_KEYMENU), which would take the focus from the field.
        if (ShortcutRecordingId() >= 0 || g_key_route.claimed_vk != 0 || TextInputNow()) return 0;
        break;

    case WM_KILLFOCUS:
        DropKeyClaims();  // spec 11-fb-3
        break;

    case WM_INPUTLANGCHANGE:
        RefreshShortcutLabels();  // spec 11-fb-3 — the key caps follow the new layout
        break;

    case kMsgRunVideoCommands:
        // Story 11-4 — the Video view's queued REAPER writes (each one undo point), run here,
        // outside the render tick, where a message box or a plug-in load may run a modal loop.
        g_video_post_pending = false;
        try {
            VideoViewRunPending();
        } catch (const std::exception& e) {
            LogError("Video view: a write failed: %s (Reaper is unaffected)", e.what());
        } catch (...) {
            LogError("Video view: a write failed (Reaper is unaffected)");
        }
        return 0;

    case kMsgLocateTextures:
        g_locate_pending = false;
        try {
            RunLocateTextures();
        } catch (const std::exception& e) {
            LogError("Locate textures failed: %s", e.what());
        } catch (...) {
            LogError("Locate textures failed");
        }
        return 0;

    case WM_DESTROY:
        // Reached only on the paths WE initiate (CloseViewerWindow → DestroyWindow,
        // and unload) — NOT the docker tab X, which merely hides us (see header). Tear
        // the render loop + GL (and ImGui) down here while the context is still current.
        StopRendering();
        g_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool EnsureClassRegistered(REAPER_PLUGIN_HINSTANCE hInst)
{
    if (g_class_registered) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC;  // each window owns its DC; required for stable WGL
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    g_class_registered = true;
    g_class_hinst      = hInst;
    return true;
}

// Reverse EnsureClassRegistered. Idempotent. Called both on the normal close/
// unload path and on a failed-open bail (the class is registered before the
// window is created, so a create-failure must unwind it to leave nothing behind).
// Uses g_class_hinst so UnregisterClassW targets the module the class was
// registered under, avoiding a stale lpfnWndProc into our image on a later load.
void UnregisterViewerClass()
{
    if (g_class_registered) {
        UnregisterClassW(kWindowClassName, g_class_hinst);
        g_class_registered = false;
        g_class_hinst      = nullptr;
    }
}

// Story 11-4 — REAPER's keyboard hook. When the viewer has the focus, REAPER would run its
// own shortcuts for every key; this hands the viewer the keys bound in src/shortcuts.h (with
// exactly their modifiers; the cut key in Video view only, in RAV view it stays REAPER's),
// Esc while the Shortcuts popup is open, and every key while a text field of the Video panel
// is active or a new key is being recorded. Other keys stay REAPER's (Space plays).
int ViewerTranslateAccel(MSG* msg, accelerator_register_t* /*ctx*/)
{
    if (!msg || !g_hwnd || !IsWindow(g_hwnd)) return 0;
    if (msg->hwnd != g_hwnd && !IsChild(g_hwnd, msg->hwnd)) return 0;
    KeyRouteInput in;
    switch (msg->message) {
    case WM_KEYDOWN:     in.msg = KeyMsg::KeyDown; break;
    case WM_SYSKEYDOWN:  in.msg = KeyMsg::SysKeyDown; break;
    case WM_KEYUP:       in.msg = KeyMsg::KeyUp; break;
    case WM_SYSKEYUP:    in.msg = KeyMsg::SysKeyUp; break;
    case WM_CHAR:        in.msg = KeyMsg::Char; break;
    case WM_SYSCHAR:     in.msg = KeyMsg::SysChar; break;
    case WM_DEADCHAR:    in.msg = KeyMsg::DeadChar; break;
    case WM_SYSDEADCHAR: in.msg = KeyMsg::SysDeadChar; break;
    case WM_MOUSEWHEEL:  in.msg = KeyMsg::Wheel; break;  // spec 11-fb-12: Alt + wheel
    default:             in.msg = KeyMsg::Other; break;
    }
    in.key        = static_cast<unsigned>(msg->wParam);
    in.repeat     = (msg->lParam & (1 << 30)) != 0;
    in.ctrl       = GetKeyState(VK_CONTROL) < 0;
    in.shift      = GetKeyState(VK_SHIFT) < 0;
    in.alt        = GetKeyState(VK_MENU) < 0;
    in.video_view = VideoViewActive();
    in.recording  = ShortcutRecordingId() >= 0;
    in.text_input = g_imgui_ready && ImGui::GetIO().WantTextInput;
    in.popup_open = ShortcutsPopupOpen() || VideoDeleteConfirmOpen();  // Esc closes either
    in.confirm_open = VideoDeleteConfirmOpen();  // Enter confirms the delete
    in.imgui_mouse = ImGuiWantsMouse();          // spec 11-fb-12: Alt + wheel over the UI
    // The decision itself is pure (src/shortcuts.h RouteViewerKey, host-tested).
    return RouteViewerKey(CurrentShortcuts(), in, g_key_route);
}

accelerator_register_t g_accel_reg = { &ViewerTranslateAccel, true, nullptr };

}  // namespace

accelerator_register_t* ViewerAcceleratorRegistration()
{
    return &g_accel_reg;
}

void OpenViewerWindow(REAPER_PLUGIN_HINSTANCE hInst, HWND reaper_main)
{
    if (g_hwnd && IsWindow(g_hwnd)) {
        // Already created (possibly hidden by a docker-X close): bring the docked
        // tab forward; the visibility-gated render loop resumes on its own.
        DockWindowActivate(g_hwnd);
        return;
    }

    if (!EnsureClassRegistered(hInst)) {
        LogError("failed to register window class");
        return;
    }

    // No startup file dialog: the viewer opens directly and shows whatever animation
    // item is under the playhead (loaded lazily by RenderTick). Animations live on the
    // timeline now — the old "choose a 3D model" launch picker was removed.
    //
    // A docked GL viewport is a WS_CHILD of Reaper's main window: the docker
    // reparents it into its own host and drives its size/visibility. It is created
    // without WS_VISIBLE and NOT self-shown — DockWindowActivate (below) reveals it
    // inside the docker. Position 0,0 is irrelevant; the docker lays it out.
    g_hwnd = CreateWindowExW(
        0, kWindowClassName, kWindowTitle,
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0,
        kInitialWidth, kInitialHeight,
        reaper_main,            // parent — the Reaper main HWND
        nullptr, hInst, nullptr);

    if (!g_hwnd) {
        // WM_CREATE returned -1 (GL/renderer init failed, already logged with a
        // specific cause) or the create failed outright. Unwind the class we just
        // registered so a failed open leaves NOTHING behind — symmetric with the
        // success path's class register↔unregister; a retry simply re-registers it.
        // Note: the plugin-entry registrations (command_id/gaccel/hookcommand/
        // toggleaction) are deliberately NOT touched here — the extension stays
        // loaded and the Action stays usable; reversing those is the unload path's
        // job, not a window-open failure's.
        LogError("the viewer window could not be created (CreateWindowExW); the viewer "
                 "did not open (Reaper is unaffected)");
        UnregisterViewerClass();
        return;
    }

    // Hand the live child to Reaper's docker (allowShow=true) and bring its tab
    // forward. identstr persists the dock position across sessions (AR15: paired
    // with DockWindowRemove on the close/unload paths we own).
    DockWindowAddEx(g_hwnd, kDockName, kDockIdent, true);
    DockWindowActivate(g_hwnd);
}

bool ViewerWindowIsVisible()
{
    return g_hwnd && IsWindow(g_hwnd) && IsWindowVisible(g_hwnd);
}

void ToggleViewerWindow(REAPER_PLUGIN_HINSTANCE hInst, HWND reaper_main)
{
    // AR18 — no-throw boundary at the host edge. Reaper calls our hookcommand
    // directly from its main message pump; a C++ exception crossing back into it
    // is undefined behaviour in the host. The renderer/loader use bool+out_error
    // (not exceptions), so nothing here *should* throw — but a stray std::bad_alloc
    // (e.g. the err string) must never take Reaper down. This single thin guard
    // covers BOTH the open and the close/toggle legs; on a caught throw we also
    // tear any half-built window down via the idempotent CloseViewerWindow().
    try {
        // A visible panel toggles off (full close); a missing or docker-hidden panel
        // toggles on. OpenViewerWindow raises an already-created-but-hidden panel via
        // DockWindowActivate, or creates + docks a fresh one.
        if (ViewerWindowIsVisible()) {
            CloseViewerWindow();
        } else {
            OpenViewerWindow(hInst, reaper_main);
        }
    } catch (const std::exception& e) {
        LogError("viewer open/close failed: %s (Reaper is unaffected)", e.what());
        CloseViewerWindow();
    } catch (...) {
        LogError("viewer open/close failed: unknown error (Reaper is unaffected)");
        CloseViewerWindow();
    }
}

void CloseViewerWindow()
{
    if (g_hwnd && IsWindow(g_hwnd)) {
        // This is a path WE own (explicit close / unload), so detach from the
        // docker first (AR15), then destroy — WM_DESTROY runs StopRendering().
        DockWindowRemove(g_hwnd);
        DestroyWindow(g_hwnd);
    } else {
        StopRendering();         // window already hidden/gone; make sure the timer/context are down
    }
    g_hwnd = nullptr;

    // Unregister the window class so a future DLL load doesn't inherit a stale
    // lpfnWndProc pointing into our unmapped image.
    UnregisterViewerClass();
}

}  // namespace rav
