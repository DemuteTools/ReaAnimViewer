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
// mesh loading is Epic 2. Story 6-5-9: full screen takes this same window out of the
// docker and gives it back, never destroying it (src/viewer_fullscreen.h).

#include "viewer_window.h"

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM — unpack WM_MOUSEMOVE coords
#include <cmath>       // std::sin/cos/asin/atan2/sqrt — light azimuth/elevation ↔ direction
#include <cfloat>      // FLT_MAX (spec 11-fb-11: the menu's height cap)
#include <cstdio>      // std::snprintf (13-14: the FPS readout)
#include <algorithm>   // std::min/max — the load message's wrap width
#include <exception>   // std::exception — the no-throw host boundary (AR18)
#include <initializer_list>  // 13-7: the UI font fallback chains
#include <filesystem>  // issue #1: the item folder for "Locate textures..."
#include <string>
#include <type_traits>    // 13-7: kUiGlyphRanges is ImWchar-compatible
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
#include "view_nav.h"          // 13-45: the timelines' wheel / zoom keys, the Shortcuts rows' recording
#include "skeleton_view.h"     // spec 10-3c: the Model / Skeleton switch and its overlay
#include "clip_list_ui.h"      // story 6-5-10: the animation clip list (left of every view)
#include "tagging_assign_ui.h"  // 13-15b: the Sound assignment window takes the keys while it has the focus
#include "tagging_session.h"   // Story 10-3: the Tagging view's model (item, rules, detection)
#include "tag_markers.h"       // Story 10-4: the markers option (Take / Project / Both)
#include "tagging_view_ui.h"   // Story 10-3: its curve strip and panel
#include "tagging_populate_ui.h"  // 13-19: Populate runs from the window procedure, with its progress
#include "busy_progress_ui.h"     // 13-59: the shared busy card of a long gesture (Populate's since 13-19)
#include "texture_locate.h"    // issue #1: "Locate textures..." (folder picker + copy)
#include "gpu_resources.h"    // Story 11-4: the Video view's render target
#include "ui_theme.h"         // Story 11-4: the DM-XYZ-Pad theme over Dear ImGui
#include "ui_flow_math.h"     // Story 6-5-9: the view switch beside the corner icons
#include "ui_flow.h"          // 13-14: LabelRow (the Light faders, the Colour row)
#include "ui_glyph_ranges.h"  // 13-7: the symbol blocks merged into the UI font
#include "video_view.h"       // Story 11-4: Video view + Video panel model
#include "video_view_ui.h"    // Story 11-4: their ImGui widgets
#include "viewer_fullscreen.h"       // Story 6-5-9: out of the docker, full screen, and back
#include "viewer_fullscreen_plan.h"  // Story 6-5-9: when Esc leaves full screen
#include "library_view_ui.h"  // Epic 13 (D20): the Library view
#include "library_node_ui.h"  // story 6-5-10: LibraryEditUsesEsc (the clip list's Esc waits for it)
#include "sound_follow.h"     // Epic 13 (D20): the menu's Sounds section (13-9)

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F   // GL 1.2; Windows <gl/GL.h> is 1.1 and may omit it
#endif

// Dear ImGui (vendored, Story 6.5.3 rev) — the in-viewport tool UI. Rendered into OUR
// GL context after the 3D scene, so it is a true overlay (no flicker, no extra window,
// no ReaImGui runtime dependency). Win32 backend feeds input via the WndProc handler;
// the OpenGL3 backend draws the widget geometry.
#include <imgui.h>
#include <imgui_internal.h>  // spec viewer-options-menu-tabs: the menu's place in the window order
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
// Story 11-7 review: the view was emptied because the items under the playhead do not play
// (muted / lane); cleared again as soon as an item plays.
bool        g_view_cleared     = false;

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
// 13-15: the viewer's ImGui context (the Sound assignment window has one of its own): our messages always
// reach this one, whichever is current when a message comes in synchronously.
ImGuiContext* g_main_imgui_ctx = nullptr;
bool  g_menu_open      = false;     // the hamburger toggles the option list
// Spec viewer-options-menu-tabs -- the menu's tabs, top to bottom (session state). The tab
// follows the view when the view changes; otherwise the picked tab stays.
// 13-17 (N6): a Library tab, opened with the Library view.
enum MenuTab { kMenuTabGlobal = 0, kMenuTabRav, kMenuTabTagging, kMenuTabVideo, kMenuTabLibrary, kMenuTabCount };
int   g_menu_tab       = kMenuTabRav;
int   g_menu_tab_view  = -1;        // the view the tab last followed (-1 = none yet)
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
constexpr ui::DpPx kStatusIconSize{22.0f};    // at the screen's DPI (13-14)
constexpr ui::DpPx kStatusIconMargin{12.0f};
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
// Both at the screen's DPI (13-14).
constexpr ui::DpPx kNavCubeSize{96.0f};
constexpr ui::DpPx kNavCubeMargin{12.0f};

// Story 11-4 — Video view. The frame is rendered into its own target at the frame's size
// (same aspect as the FX output, so the same framing), then copied into the frame rect of
// the window. Session state; freed with the GL context in StopRendering.
constexpr UINT kMsgRunVideoCommands = WM_APP + 0x31;  // runs the Video view's queued writes
// Story 10-3b -- the Tagging view's preset Import / Export pickers (a modal loop: window
// procedure only); the flag stops a second post meanwhile.
constexpr UINT kMsgTaggingDialog = WM_APP + 0x33;
bool            g_tagging_dialog_pending = false;
// 13-19: a Populate / Restore clicked in the panel runs from the window procedure (outside the render
// tick and its ImGui frame), drawing its progress frames; the flag stops a second post meanwhile.
constexpr UINT kMsgRunPopulate = WM_APP + 0x34;
bool           g_populate_post_pending = false;
// Story 6-5-9 -- full screen. The button's click changes the window's mode from the window
// procedure (never inside the render tick, which is inside the ImGui frame); the flag stops a
// second post meanwhile. REAPER's main window: the parent of the docked child, the owner of the
// full screen window. The action's id, to refresh its toolbar button on every change.
constexpr UINT kMsgFullScreen = WM_APP + 0x59;
bool           g_full_screen_post_pending = false;
HWND           g_reaper_main_hwnd = nullptr;
int            g_full_screen_cmd = 0;
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
bool PaintBusyCard(const BusyFrame& f);  // 13-59: defined below, set as the busy painter at init
void DropBusyInputs();

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
    // Story 10-3: Tagging view's curve strip takes the bottom band the same way.
    if (TaggingViewActive()) return TaggingStripHeight(static_cast<float>(g_client_h)) + 2.0f * kTaggingGap;
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

// Story 10-3 -- Tagging view: RAV view's free camera and render, in the area left of the
// panel and above the strip (rendered into the view target, then blitted there). No second
// camera, no second render. Outside the area = the theme's background (the strip and the
// panel are drawn over it).
void RenderTaggingView(float anim_time, bool loop)
{
    const int aw = std::max(1, static_cast<int>(static_cast<float>(g_client_w) - TaggingPanelFootprint(g_client_w)));
    const int ah = std::max(1, static_cast<int>(ViewBottom()));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_client_w, g_client_h);
    const ImVec4 outside = ui::Col(ui::kBg);
    glClearColor(outside.x, outside.y, outside.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (EnsureVideoTarget(aw, ah)) {
        g_renderer.RenderFrame(anim_time, loop, aw, ah, g_video_fbo.get());
        const int dy = g_client_h - ah;  // GL rows count from the bottom: the area's top is the window's
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_video_fbo.get());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, aw, ah, 0, dy, aw, dy + ah, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, g_client_w, g_client_h);
    } else {
        // No target (the GPU refused it): the whole window, as RAV view.
        g_renderer.RenderFrame(anim_time, loop, g_client_w, g_client_h);
    }
}

// 13-19 review: true while the queued Populate runs (kMsgRunPopulate). REAPER may pump messages
// during it; no frame runs inside the gesture (only the progress painter draws), and a nested
// kMsgRunPopulate is ignored.
static bool g_populate_running = false;

void RenderTick()
{
    if (!g_hdc || !g_hglrc) return;
    if (g_populate_running) return;
    // 13-8 fix (Antho 2026-10-12, a Switch pick hung REAPER): a gesture run from this frame may
    // make REAPER pump messages (a dialog REAPER opens); REAPER's timer then calls us again
    // inside the frame, and a second ImGui frame inside the first breaks both. That nested tick is
    // skipped (said once in the log); the outer frame finishes as usual.
    static bool s_in_tick = false;
    static bool s_said = false;
    if (s_in_tick) {
        if (!s_said) LogInfo("viewer: a frame was asked for inside a frame (a REAPER dialog during a gesture); skipped");
        s_said = true;
        return;
    }
    s_in_tick = true;
    struct LeaveTick {
        ~LeaveTick() { s_in_tick = false; }
    } leave_tick;

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
    MediaItem*  item_media = nullptr;  // Story 10-3: the Tagging view reads its rules
    bool        item_skipped = false;  // Story 11-7: items here, none playing (muted / lane)
    const bool item_found = GetCurrentAnimItemOn(VideoViewPinnedTrack(), item_path, item_time, &item_track,
                                                 &item_media, &item_skipped);
    if (item_found) g_view_cleared = false;
    ClipListSetCurrentItem(item_found ? item_media : nullptr);  // story 6-5-10: the lit row
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
    // Story 11-7 review: but when the only items under the playhead are muted or on lanes
    // that do not play, REAPER shows nothing there: the view clears (once; the Tagging view
    // drops the item too), and the item's file loads again when one plays.
    else if (item_skipped && !g_view_cleared) {
        g_view_cleared = true;
        g_renderer.SetAsset(Asset{});
        g_current_anim_path.clear();
        g_current_load_ok = false;
        g_missing_tex_item.clear();
        g_missing_tex_files.clear();
    }

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
    // Story 10-3: leaving Tagging view (V, the toggle, P) ends its running gesture (a drag is
    // written as shown, one undo point); in it, its model follows the item shown.
    {
        static bool s_was_tagging = false;
        const bool tagging = TaggingViewActive();
        if (s_was_tagging && !tagging) TaggingEndGestures();
        s_was_tagging = tagging;
        if (tagging) TaggingSessionFrame(item_found ? item_media : nullptr, item_found ? item_path : std::string());
    }

    // Spec 10-3c: Skeleton mode fades the model in RAV view and Tagging view (never Video view,
    // which renders the picture as the video FX does). Model mode = alpha 1, unchanged pixels.
    g_renderer.SetModelAlpha((!VideoViewActive() && SkeletonModeOn()) ? kSkeletonModelAlpha : 1.0f);

    if (VideoViewActive()) {
        RenderVideoView(static_cast<float>(g_display_time));
    } else if (TaggingViewActive()) {
        RenderTaggingView(static_cast<float>(g_transport_driven ? g_display_time : ElapsedSeconds()), !g_transport_driven);
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

    BusyTakePainted();  // 13-59: a card painted before this frame (Populate, the follow) is over
    DrawToolUi();   // Dear ImGui overlay on top of the 3D scene (Story 6.5.3 rev)

    // 13-59: a long gesture run from this frame painted its busy card over the back buffer: this
    // frame is not presented (the next one is whole).
    if (!BusyTakePainted()) SwapBuffers(g_hdc);

    // 13-15: the Sound assignment window, a window of its own anywhere on the screen (its own ImGui
    // context on the shared GL context); it puts our context back on return.
    if (g_imgui_ready) DrawAssignFloat(g_hwnd, g_hdc, g_hglrc);

    // Story 11-4 — the Video view's REAPER writes run from the window procedure, never
    // inside this tick (a message box or a plug-in load may run a modal loop).
    if (VideoViewHasPending() && !g_video_post_pending && g_hwnd)
        g_video_post_pending = PostMessageW(g_hwnd, kMsgRunVideoCommands, 0, 0) != FALSE;
    if (TaggingHasPendingDialog() && !g_tagging_dialog_pending && g_hwnd)
        g_tagging_dialog_pending = PostMessageW(g_hwnd, kMsgTaggingDialog, 0, 0) != FALSE;
    if (PopulateHasPending() && !g_populate_post_pending && g_hwnd)
        g_populate_post_pending = PostMessageW(g_hwnd, kMsgRunPopulate, 0, 0) != FALSE;

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
            HideAssignFloat();  // 13-15: no tick runs while hidden: the assignment window hides with the panel
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
    if (g_populate_running) return;  // 13-19 review: no frame inside the Populate gesture
    RenderTick();
}

// Spec viewer-options-menu-tabs -- puts the menu in front of every viewer window, then every
// open popup / modal / tooltip back in front of it (their order kept). ImGui draws and hovers
// windows in g.Windows order (the last on top); a click brings its window to the end. Each
// root is followed by its active child windows (the menu's scrolling tab content), else the
// hover test would find the root above its own children.
void BringRootAndChildrenToFront(ImGuiWindow* root)
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImVector<ImGuiWindow*> children;
    for (ImGuiWindow* w : g.Windows)
        if (w != root && w->Active && w->RootWindow == root) children.push_back(w);
    ImGui::BringWindowToDisplayFront(root);
    for (ImGuiWindow* w : children) ImGui::BringWindowToDisplayFront(w);
}

void KeepMenuInFront(ImGuiWindow* menu)
{
    if (!menu) return;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImVector<ImGuiWindow*> above;
    for (ImGuiWindow* w : g.Windows) {
        if (w == menu || !w->Active || (w->Flags & ImGuiWindowFlags_ChildWindow)) continue;
        if (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) above.push_back(w);
    }
    BringRootAndChildrenToFront(menu);
    for (ImGuiWindow* w : above) BringRootAndChildrenToFront(w);
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
    const float R = diameter * 0.5f - ui::Dp(4.0f);

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
    dl->AddCircleFilled(c, R, ui::kBg, 48);
    dl->AddCircle(c, R, ui::kStrokeStrong, 48, ui::Dp(1.5f));
    dl->AddCircle(c, R * 0.5f, ui::kStroke, 40, 1.0f);
    dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), ui::kStroke);
    dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), ui::kStroke);

    // Handle, placed from the current azimuth/elevation.
    const float rho = (kHalfPi - g_light_elevation) / kPiF;    // 0..1
    const ImVec2 h(c.x + std::sin(g_light_azimuth) * rho * R,
                   c.y - std::cos(g_light_azimuth) * rho * R);
    dl->AddCircleFilled(h, ui::Dp(6.0f), ui::kAccent, 16);
    dl->AddCircle(h, ui::Dp(6.0f), ui::kWhite, 16, ui::Dp(1.5f));
    return changed;
}

// The load message's content — title (red/amber), one bullet per line, "Copy details" —
// drawn into the CURRENT ImGui window. Shared by the transient top-centre message and the
// status icon's hover panel so both always say the same thing. Text wraps so a narrow
// docked panel still shows it.
void DrawLoadMessageBody()
{
    const float wrap_w = std::max(ui::Dp(160.0f), std::min(ui::Dp(480.0f), g_client_w - ui::Dp(60.0f)));
    const ImU32 title_col = g_load_msg_error ? ui::kBad : g_load_msg_info ? ui::kOk : ui::kWarn;
    // One wrap edge for the title and every bullet (ToneText wraps within it).
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
    ui::ToneText(g_load_msg_title, title_col);
    for (const std::string& line : g_load_msg_report) {   // issue #1: the locate outcome
        ImGui::Bullet();
        ui::ToneText(line, ui::kOk);
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
    if (ui::SolidButton(copied ? "Copied!##copyerr" : "Copy details##copyerr"))
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

    const float icon_top = ViewBottom() - kStatusIconMargin - kStatusIconSize;  // Story 11-5: above the strip
    bool icon_hovered = false;
    if (ui::BeginOverlay("##statusicon", ImVec2(kStatusIconMargin, icon_top))) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##statushit", ImVec2(kStatusIconSize, kStatusIconSize));
        icon_hovered = ImGui::IsItemHovered();

        const float  s  = kStatusIconSize;
        const float  cx = p0.x + s * 0.5f;
        const float  bar = ui::Dp(1.5f), dot = ui::Dp(1.9f), in = ui::Dp(1.0f);  // the "!"'s bar and dot
        if (g_load_msg_error) {
            const ImU32 ink = ui::kWhite;
            dl->AddCircleFilled(ImVec2(cx, p0.y + s * 0.5f), s * 0.5f, ui::kBad, 24);
            dl->AddRectFilled(ImVec2(cx - bar, p0.y + s * 0.22f), ImVec2(cx + bar, p0.y + s * 0.60f), ink, in);
            dl->AddCircleFilled(ImVec2(cx, p0.y + s * 0.75f), dot, ink, 8);
        } else {
            const ImU32 ink = ui::kConfirmActText;  // dark ink on amber
            dl->AddTriangleFilled(ImVec2(cx, p0.y + in), ImVec2(p0.x + s - in, p0.y + s - 2.0f * in),
                                  ImVec2(p0.x + in, p0.y + s - 2.0f * in), ui::kWarn);
            dl->AddRectFilled(ImVec2(cx - bar, p0.y + s * 0.34f), ImVec2(cx + bar, p0.y + s * 0.64f), ink, in);
            dl->AddCircleFilled(ImVec2(cx, p0.y + s * 0.78f), dot, ink, 8);
        }
    }
    ui::EndOverlay();

    const double now = ElapsedSeconds();
    if (icon_hovered) g_status_panel_until = now + kStatusPanelGrace;
    if (now >= g_status_panel_until) return;

    // The panel's bottom edge sits just above the icon (pivot bottom-left), so the mouse
    // crosses only a couple of pixels — covered by the grace — to reach "Copy details".
    // 13-14: the theme's floating card.
    if (ui::BeginFloatingCard("##statuspanel", ImVec2(kStatusIconMargin, icon_top - ui::Dp(2.0f)), ImVec2(0.0f, 1.0f))) {
        DrawLoadMessageBody();
        if (ImGui::IsWindowHovered()) g_status_panel_until = now + kStatusPanelGrace;
    }
    ui::EndFloatingCard();
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
        { { 1, 0, 0}, {0, 1, 0}, {0, 0, 1}, ui::kAxisX },      // +X  red
        { {-1, 0, 0}, {0, 1, 0}, {0, 0, 1}, ui::kAxisXDark },  // -X  dark red
        { { 0, 1, 0}, {1, 0, 0}, {0, 0, 1}, ui::kAxisY },      // +Y  green
        { { 0,-1, 0}, {1, 0, 0}, {0, 0, 1}, ui::kAxisYDark },  // -Y  dark green
        { { 0, 0, 1}, {1, 0, 0}, {0, 1, 0}, ui::kAxisZ },      // +Z  blue
        { { 0, 0,-1}, {1, 0, 0}, {0, 1, 0}, ui::kAxisZDark },  // -Z  dark blue
    };

    // Bottom-right pivot (1,1) anchored to the live client size (current in WM_SIZE) — mirrors
    // the FPS read-out's right-edge pivot but pinned to the bottom-right corner. Just the cube
    // floats over the viewport (ui::BeginOverlay: no background).
    if (!ui::BeginOverlay("##navcube", ImVec2(right_x - kNavCubeMargin, bottom_y - kNavCubeMargin),  // Story 11-5: above the shot strip
                          ImVec2(1.0f, 1.0f))) {
        ui::EndOverlay();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    // The hit area covers the whole square; IsItemHovered gates picking so the cube only
    // grabs the mouse over its own region (the camera-vs-ImGui arbitration sees WantCaptureMouse).
    const bool pressed = ImGui::InvisibleButton("##navcubehit", ImVec2(kNavCubeSize, kNavCubeSize));
    const bool hovered = ImGui::IsItemHovered();

    const ImVec2 c(p0.x + kNavCubeSize * 0.5f, p0.y + kNavCubeSize * 0.5f);
    const float scale = (kNavCubeSize * 0.5f - ui::Dp(6.0f)) / 1.85f;  // fit the sqrt(3) cube corner inside

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
        dl->AddPolyline(quad, 4, ui::kNavEdge, ImDrawFlags_Closed, ui::Dp(1.5f));

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
        const ImU32 kHi = ui::kHighlight;   // amber highlight (matches the reference)

        if (!iEdge && !jEdge) {
            // FACE — translucent fill + outline over the whole face.
            const ImVec2 q[4] = { project(f.n - f.u - f.v), project(f.n + f.u - f.v),
                                  project(f.n + f.u + f.v), project(f.n - f.u + f.v) };
            dl->AddConvexPolyFilled(q, 4, ui::WithAlpha(ui::kHighlight, 90));
            dl->AddPolyline(q, 4, kHi, ImDrawFlags_Closed, ui::Dp(2.0f));
        } else if (iEdge && jEdge) {
            // CORNER — a dot at the cube corner.
            const ImVec2 cc = project(f.n + float(hitI - 1) * f.u + float(hitJ - 1) * f.v);
            dl->AddCircleFilled(cc, ui::Dp(7.0f), kHi, 16);
            dl->AddCircle(cc, ui::Dp(7.0f), ui::kConfirmActText, 16, ui::Dp(1.5f));
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
            dl->AddLine(a, b, kHi, ui::Dp(6.0f));
            dl->AddCircleFilled(a, ui::Dp(3.0f), kHi, 12);
            dl->AddCircleFilled(b, ui::Dp(3.0f), kHi, 12);
        }

        if (pressed) {
            if (video) QueueVideoSnap(hitDir);
            else       g_renderer.SnapCameraTo(hitDir);
        }
    }

    ui::EndOverlay();
}

// Builds + renders the Dear ImGui tool UI for this frame, on top of the 3D scene (called
// between RenderFrame and SwapBuffers). A FRAMELESS hamburger menu (Antho's icons) that
// shows / hides the options panel: REAPER-style vertical tabs (Global, RAV view, Tagging
// view, Video view), the picked tab's settings beside them, a footer (Copy error log,
// version). Fixed top-left, non-resizable, drawn above every other viewer window. All
// widgets mutate state only and push to the renderer (no synchronous re-render, AR18).
// NON-fatal: if ImGui failed to initialize this is a no-op and the viewport still runs.
void DrawToolUi()
{
    if (!g_imgui_ready) return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // 13-60b: reaper-kb.ini is checked each frame (at most once a second), not only while a timeline is
    // drawn: a KEYS row on REAPER follows a key just given in REAPER's Actions window in any view.
    CurrentViewNav();

    // Spec viewer-options-menu-tabs: the menu's tab follows the view when it changes (toggle
    // or V); otherwise the picked tab stays.
    {
        const ViewMode vm = GetViewMode();
        if (g_menu_tab_view != static_cast<int>(vm)) {
            g_menu_tab_view = static_cast<int>(vm);
            g_menu_tab = vm == ViewMode::Tagging   ? kMenuTabTagging
                         : vm == ViewMode::Video   ? kMenuTabVideo
                         : vm == ViewMode::Library ? kMenuTabLibrary
                                                   : kMenuTabRav;
        }
    }
    // Story 10-4: the Tagging panel's "Change" (markers written) opens the menu on its tab.
    if (TaggingConsumeMenuRequest()) {
        g_menu_open = true;
        g_menu_tab = kMenuTabTagging;
    }

    // Spec 11-fb-11: open, the menu stops 10 px above the window's bottom (its tab content
    // scrolls, the tab column stays); closed, it is the bare button.
    const float kMenuMargin = ui::Dp(10.0f);  // its gap to the window's top and bottom edges
    ImGui::SetNextWindowPos(ImVec2(kMenuMargin, kMenuMargin), ImGuiCond_Always);
    const float menu_max_h = std::max(ui::Dp(48.0f), static_cast<float>(g_client_h) - 2.0f * kMenuMargin);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
    // Open, the whole menu can still scroll on a viewer too short for its tabs and footer.
    constexpr ImGuiWindowFlags kClosedFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, menu_max_h));

    // Story 11-4: the theme (ui_theme.h) styles the menu. Open, it is a panel like the
    // Tagging and Video panels (ui::PushPanelStyle), its settings in ui::Section cards.
    // Closed, the bare button over the 3D view (ui::PushOverlayPanelStyle).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Dp(8.0f), ui::Dp(8.0f)));
    if (g_menu_open) {
        ui::PushPanelStyle();
    } else {
        ui::PushOverlayPanelStyle();
    }
    ImGui::Begin("##tools", nullptr, g_menu_open ? kFlags : (kFlags | kClosedFlags));
    ImGuiWindow* const menu_window = ImGui::GetCurrentWindow();

    // Hamburger button — frameless icon (the window behind is its frame); toggles the option list.
    // 13-7 (audit C0.3): the mockup's burger is 28 x 26, its 22 px icon bright (kText).
    {
        ui::IconOnlyOpts o;
        o.w = ui::Dp(28.0f);
        o.h = ui::Dp(26.0f);
        o.icon_px = ui::Dp(22.0f);
        o.tint = ui::kText;
        if (ui::IconOnlyButton("##menu", IconTex(g_icon_menu), g_menu_open ? "Close the options" : "Options", o))
            g_menu_open = !g_menu_open;
    }

    if (g_menu_open) {
        const ImGuiStyle& st = ImGui::GetStyle();
        ImGui::Spacing();

        // Spec viewer-options-menu-tabs: REAPER-style vertical tabs on the left (Global, then one
        // per view), the selected tab's settings on the right, a fixed footer under both.
        static const char* const kTabLabels[kMenuTabCount] = { "Global", "Main", "Tagging", "Video", "Library" };
        const float tabs_top = ImGui::GetCursorScreenPos().y;
        ui::VerticalTabs("##menutabs", kTabLabels, kMenuTabCount, &g_menu_tab);
        const float tabs_bottom = ImGui::GetItemRectMax().y;
        const float divider_x = ImGui::GetItemRectMax().x + ui::Dp(7.0f);  // between the tabs and the content
        ImGui::SameLine(0.0f, ui::Dp(14.0f));

        // The content scrolls within what the window's height leaves after the header and the
        // footer (separator, one button row, the version line, the bottom padding).
        const float kContentW = ui::Dp(256.0f);
        const float footer_h = 2.0f * st.ItemSpacing.y + 1.0f + ImGui::GetFrameHeightWithSpacing() +
                               ui::Dp(4.0f) + st.ItemSpacing.y + ImGui::GetTextLineHeight() + st.WindowPadding.y;
        const float content_max_h =
            std::max(tabs_bottom - tabs_top, kMenuMargin + menu_max_h - tabs_top - footer_h);
        ImGui::SetNextWindowSizeConstraints(ImVec2(kContentW, 0.0f), ImVec2(kContentW, content_max_h));
        // No fill of its own: the menu's panel shows through.
        ImGui::BeginChild("##menutab", ImVec2(kContentW, 0.0f), ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoBackground);

        // Every group of settings is a ui::Section (the theme's card with its header), so each
        // tab reads the same way and a new setting needs no styling of its own.
        ui::Section sec;

        switch (g_menu_tab) {
        case kMenuTabGlobal: {
        ui::SubText("Shared by the three views.");

        // --- Light: Colour + Position ---
        if (ui::BeginSection(sec, "Light", IconTex(g_icon_light))) {
            // 13-14 (G-12): a swatch that opens the themed picker, in a labelled row.
            ui::LabelRow("Colour", ui::ControlH());
            // The picker keeps the RGB / HSV / Hex fields and the Current / Original preview.
            ui::ColorSwatchOpts copts;
            copts.inputs = true;
            copts.original = true;
            // 13-37: every setting of the menu says what it changes, in a tip.
            if (ui::ColorSwatchButton("##colour", g_light_color, "The colour of the light on the model", copts))
                g_renderer.SetLightColor(glm::vec3(g_light_color[0], g_light_color[1], g_light_color[2]));

            ImGui::Dummy(ImVec2(0.0f, ui::Dp(3.0f)));
            ui::Caption("Position");
            if (LightDirectionPad(ui::Dp(150.0f)))
                g_renderer.SetLightDir(LightDirFromAngles(g_light_azimuth, g_light_elevation));
            ui::ItemTipText("Drag the dot to move the light around the model.\n"
                            "Centre: straight above. Middle ring: level with the model. Edge: below.\n"
                            "The shadow on the floor follows.");

            // Lighting-quality knobs to fix the "flat/cheap vs Mixamo" look in-Reaper, live:
            // Ambient = fill (lower → more contrast), Specular = sheen, Relief = normal-map boost.
            // 13-14 (G-13): labelled faders, the value drawn after the track.
            ImGui::Dummy(ImVec2(0.0f, ui::Dp(3.0f)));
            ui::FaderOpts fo;
            fo.format = "%.2f";
            if (ui::Fader("##ambient", &g_ambient, 0.0f, 1.0f, ui::LabelRow("Ambient", ui::Dp(100.0f)), fo))
                g_renderer.SetAmbient(g_ambient);
            ui::ItemTipText("How much light reaches the side of the model the light does not face.\n"
                            "Lower: darker shadows, more contrast.");
            if (ui::Fader("##specular", &g_spec_strength, 0.0f, 2.0f, ui::LabelRow("Specular", ui::Dp(100.0f)), fo))
                g_renderer.SetSpecStrength(g_spec_strength);
            ui::ItemTipText("How shiny the model looks: the strength of the light's highlights.");
            if (ui::Fader("##relief", &g_normal_strength, 0.0f, 3.0f, ui::LabelRow("Relief", ui::Dp(100.0f)), fo))
                g_renderer.SetNormalStrength(g_normal_strength);
            ui::ItemTipText("How deep the surface detail of the textures looks (bumps, folds, seams).\n"
                            "0: flat. Needs Normal maps on (Performance).");
        }
        ui::EndSection(sec);

        // --- Ground: show/hide the floor + grid (hiding it also drops the cast shadow,
        // the floor being the only receiver) ---
        if (ui::BeginSection(sec, "Ground", IconTex(g_icon_ground))) {
            if (ui::Checkbox("Enable", &g_floor_visible))
                g_renderer.SetFloorVisible(g_floor_visible);
            ui::ItemTipText("Shows the floor and its grid under the model.\n"
                            "Off also removes the shadow, which falls on the floor.");
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
            ui::ItemTipText("The size of one grid square, in real metres, to judge distances and speed.");
        }
        ui::EndSection(sec);

        // --- Shadow: cast-shadow quality (Off skips the depth pass; no floor → no shadow) ---
        if (ui::BeginSection(sec, "Shadow", IconTex(g_icon_shadow))) {
            static const char* const kShadowLabels[4] = { "Off", "Low", "Mid", "High" };
            if (ui::Segmented("##shadow", kShadowLabels, 4, &g_shadow_quality, 0.0f, -1))
                g_renderer.SetShadowQuality(static_cast<ShadowQuality>(g_shadow_quality));
            ui::ItemTipText("The shadow the model casts on the floor.\n"
                            "Higher: sharper, smoother edges, more work for the graphics card. Off: no shadow, fastest.");
        }
        ui::EndSection(sec);

        // --- Performance: render-quality levers (FR52) + on-canvas FPS (FR53). Antho's
        // Performance icon. The Ground (floor) + Shadow levers live in their own sections above
        // and TOGETHER with these complete the FR52 set — not duplicated here. ---
        if (ui::BeginSection(sec, "Performance", IconTex(g_icon_performance))) {

            if (ui::Checkbox("Normal maps", &g_normal_maps_on))
                g_renderer.SetNormalMapsEnabled(g_normal_maps_on);
            ui::ItemTipText("Shows the surface detail painted in the model's textures (bumps, folds, seams).\n"
                            "Off: smoother look, a little faster.");

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
            ui::ItemTipText("Smooths the jagged edges of the model.\n"
                            "Higher: smoother, more work for the graphics card. The greyed level and those above it are beyond your card.");

            ui::Checkbox("FPS", &g_fps_overlay_on);  // pure UI state — read by the overlay below
            ui::ItemTipText("Shows at the top right of the view how many images per second it draws.\n"
                            "Low numbers: lower Shadow or MSAA.");
        }
        ui::EndSection(sec);
        break;
        }

        // --- RAV view: the camera, and the Model / Skeleton switch's state (the same as the
        // switch at the bottom left: either one changes both) ---
        case kMenuTabRav: {
            if (ui::BeginSection(sec, "Camera")) {
                if (ui::SolidButton("Recenter camera", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
                    g_renderer.ResetCamera();
                ui::ItemTipText("Puts the camera back to its starting view, framing the whole model.");
            }
            ui::EndSection(sec);
            if (ui::BeginSection(sec, "Model / Skeleton")) {
                static const char* const kModes[2] = { "Model", "Skeleton" };
                int skel_sel = SkeletonModeOn() ? 1 : 0;
                if (ui::Segmented("##menuskelmode", kModes, 2, &skel_sel)) SetSkeletonModeOn(skel_sel == 1);
                ui::ItemTipText("What the view shows: the model, or its bones (click a bone to pick it for a rule).\n"
                                "The same as the switch at the bottom left.");
            }
            ui::EndSection(sec);
            break;
        }

        // --- Story 10-4: Tagging view -- which markers Commit writes (a global option, kept
        // across sessions; project markers take the rule's colour) ---
        case kMenuTabTagging: {
            if (ui::BeginSection(sec, "Markers written")) {
                static const char* const kMarkLabels[3] = { "Take", "Project", "Both" };
                const MarkerMode mode = GetTaggingMarkerMode();
                int mark_sel = mode == MarkerMode::Take ? 0 : mode == MarkerMode::Project ? 1 : 2;
                if (ui::Segmented("##tagmarkers", kMarkLabels, 3, &mark_sel, 0.0f, -1))
                    SetTaggingMarkerMode(mark_sel == 0 ? MarkerMode::Take : mark_sel == 1 ? MarkerMode::Project : MarkerMode::Both);
                ui::ItemTip("Where Commit writes the events it detects: on the take (moves with the item), on the\n"
                            "project's timeline, or both. %s (previews too).\nProject markers take the rule's colour.",
                            MarkerModeLine(GetTaggingMarkerMode()));
            }
            ui::EndSection(sec);
            // Epic 13 (D20): the Sounds section of the menu (13-9).
            DrawSoundsMenuSection();
            break;
        }

        // --- Story 13-17 (N6): Library view -- the tracks placed sounds land on ---
        case kMenuTabLibrary: {
            DrawLibraryTracksSection();
            break;
        }

        // --- Video view: the Video panel (Story 11-4), the Delete key's question (spec
        // 11-fb-11), REAPER catch-up (spec 11-fb-5) ---
        default: {
            if (ui::BeginSection(sec, "Panel")) {
                bool panel = VideoPanelVisible();
                if (ui::Checkbox("Video panel", &panel)) SetVideoPanelVisible(panel);
                ui::ItemTipText("Shows the Video panel on the right of Video view (shots, camera, render).\n"
                                "The same as the panel button at the top right.");
                // Spec 11-fb-11 -- the Delete key asks first unless "Don't ask again" was ticked;
                // this gives the question back (or takes it away).
                bool ask = VideoAskBeforeDeleteShot();
                if (ui::Checkbox("Ask before deleting a shot", &ask)) SetVideoAskBeforeDeleteShot(ask);
                ui::ItemTip("The \"Delete current shot\" shortcut%s in Video view deletes the current shot: ask "
                            "first, or delete at once",
                            ShortcutSuffix(ShortcutKeyLabel(kShortcutDeleteShot)).c_str());
            }
            ui::EndSection(sec);
            if (ui::BeginSection(sec, "REAPER's Video window")) {
                // How long REAPER's Video window takes to catch up with an output size or display
                // change while playing, on Video view's caption line (spec 11-fb-5).
                ui::Checkbox("REAPER catch-up", &g_preview_lag_on);
                ui::ItemTipText("Shows on Video view's caption line how long REAPER's Video window takes to catch up\n"
                                "after an output size or display change while playing.");
            }
            ui::EndSection(sec);
            break;
        }
        }

        ImGui::EndChild();
        // The line between the tab column and the content.
        ImGui::GetWindowDrawList()->AddLine(ImVec2(divider_x, tabs_top),
                                            ImVec2(divider_x, std::max(tabs_bottom, ImGui::GetItemRectMax().y)),
                                            ui::kStroke, 1.0f);

        // The footer, under the tabs and the content.
        ImGui::Separator();
        // Always enabled: even with no error kept, the GPU line helps a bug report.
        const bool copied = ElapsedSeconds() < g_copied_until;
        if (ui::SolidButton(copied ? "Copied!##copylog" : "Copy error log##copylog", ImVec2(ui::Dp(196.0f), 0.0f)))
            CopyErrorLogToClipboard();
        ui::ItemTipText("Copies RAV's recent errors and your graphics card's name, to paste in a bug report.");

        // Build version (a dev build shows "<last release>-dev+<commit>"), so a user can tell
        // which one they run.
        ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
        ui::Caption("Version " RAV_DISPLAY_VERSION);
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
    // Story 10-3: or the Tagging panel's, in Tagging view (each is 0 outside its view).
    const bool  video_view = VideoViewActive();
    const bool  tagging_view = TaggingViewActive();
    const float panel_room = VideoPanelFootprint(g_client_w) + TaggingPanelFootprint(g_client_w);
    const float area_right = static_cast<float>(g_client_w) - panel_room;

    // FPS readout, top-RIGHT (FR53), anchored with a right-edge pivot so it hugs the corner at
    // any client size. This REPLACES the console FPS log removed in 6.5.2 — no console output.
    // Story 11-4: at the view area's corner, left of the panel button in Video view.
    // 13-14 (C0.4): hidden in the Library view (no 3D view to measure); the other views keep it.
    const bool        library_view = LibraryViewActive();
    const float       corner_x = area_right - ui::Dp(video_view ? 46.0f : 10.0f);
    const bool        fps_shown = g_fps_overlay_on && !library_view;
    const ImGuiStyle& st = ImGui::GetStyle();
    // A fixed slot ("000 FPS") so the icons do not jitter as the digits change.
    const float fps_w = fps_shown ? ImGui::CalcTextSize("000 FPS").x + 2.0f * st.WindowPadding.x + ui::Dp(2.0f) : 0.0f;
    const float keys_w = ui::Dp(24.0f) + ui::Dp(2.0f);  // the keyboard icon's 24 px button + its gap
    // Story 6-5-9 -- the top row adapts to its width: the view switch keeps the middle while the
    // corner group (FPS, keyboard, full screen) fits beside it, moves left to make room, and when
    // even that does not fit the corner group goes under the switch (ui_flow_math, host-tested).
    // The switch's size is the one it had last frame (it does not depend on where it is drawn).
    const ImVec2 toggle_size = VideoViewToggleSize();
    bool         corner_below = false;
    // Never left of the closed menu button's card (x 10, 28 px button + 2 x 8 padding) + 8 px.
    const float  toggle_x = flow::CentreBesideTrail(ui::Dp(10.0f + 44.0f + 8.0f), area_right * 0.5f, toggle_size.x,
                                                    fps_w + keys_w + ui::Dp(24.0f), corner_x, ui::Dp(8.0f),
                                                    &corner_below);
    const float readout_top = corner_below ? ui::Dp(8.0f) + toggle_size.y + ui::Dp(4.0f) : ui::Dp(14.0f);
    const float text_mid = readout_top + st.WindowPadding.y + ImGui::GetTextLineHeight() * 0.5f;

    // Epic 13 (D20, 13-2) -- Library view: one full-area window under the view toggle, drawn
    // before the readouts and overlays (which stay on top of it). Story 6-5-9: under the corner
    // icons too when they went under the switch.
    if (library_view) {
        const float lib_top = std::max(ui::Dp(44.0f), corner_below ? text_mid + ui::Dp(12.0f + 6.0f) : 0.0f);
        DrawLibraryView(ui::Dp(8.0f), lib_top, std::max(1.0f, static_cast<float>(g_client_w) - ui::Dp(16.0f)),
                        std::max(1.0f, static_cast<float>(g_client_h) - lib_top - ui::Dp(8.0f)));
    }

    if (fps_shown) {
        ImGui::SetNextWindowPos(ImVec2(corner_x, readout_top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        if (ImGui::Begin("##fps", nullptr, kReadoutFlags)) {
            char fps_text[32];
            std::snprintf(fps_text, sizeof(fps_text), "%.0f FPS", g_fps);
            ui::ToneText(fps_text, ui::kMuted);
        }
        ImGui::End();
    }

    // Spec 11-fb-3 — the keyboard icon (Shortcuts popup), left of the FPS readout, centred on
    // its text line; at the corner when the readout is hidden.
    DrawShortcutsButton(corner_x - fps_w, text_mid, IconTex(g_icon_keyboard));
    // Story 6-5-9 -- the Full screen button, left of the keyboard icon (the same 24 px button).
    // Its click is posted: the window changes mode from the window procedure, not this frame.
    if (DrawFullScreenButton(corner_x - fps_w - keys_w, text_mid, ShortcutKeyLabel(kShortcutFullScreen)) &&
        !g_full_screen_post_pending && g_hwnd)
        g_full_screen_post_pending = PostMessageW(g_hwnd, kMsgFullScreen, 0, 0) != FALSE;

    // Story 11-4 — RAV view / Video view toggle (top centre of the view area, key V); in
    // Video view the frame's edge + captions, its state message, and the panel button.
    DrawVideoViewToggle(toggle_x);
    if (video_view) {
        DrawVideoFrameDecor(g_video_frame, &CopyErrorLogToClipboard, g_preview_lag_on);
        DrawVideoPanelButton(area_right - ui::Dp(8.0f));
        // Story 11-5 -- the shot strip under the viewport (UX decision 4).
        if (VideoStripBand() > 0.0f) {
            const float strip_h = VideoStripHeight(static_cast<float>(g_client_h));
            DrawVideoShotStrip(kVideoStripGap, static_cast<float>(g_client_h) - kVideoStripGap - strip_h,
                               std::max(1.0f, area_right - 2.0f * kVideoStripGap), strip_h);
        }
    }
    // Story 10-3 -- Tagging view: the curve strip under the 3D view, the panel on the right.
    if (tagging_view) {
        const float strip_h = TaggingStripHeight(static_cast<float>(g_client_h));
        DrawTaggingStrip(kTaggingGap, static_cast<float>(g_client_h) - kTaggingGap - strip_h,
                         std::max(1.0f, area_right - 2.0f * kTaggingGap), strip_h);
        DrawTaggingPanel(area_right + kTaggingGap, kTaggingGap, panel_room - 2.0f * kTaggingGap,
                         std::max(1.0f, static_cast<float>(g_client_h) - 2.0f * kTaggingGap));
    }
    if (video_view && panel_room > 0.0f) {
        const float gap = ui::Dp(8.0f);
        DrawVideoPanel(area_right + gap, gap, panel_room - 2.0f * gap,
                       std::max(1.0f, static_cast<float>(g_client_h) - 2.0f * gap), g_renderer.Camera(),
                       &CopyErrorLogToClipboard);
    }
    // Spec 11-fb-11 -- the Delete key's confirmation (closes itself outside Video view).
    DrawVideoDeleteConfirm();

    // Spec 10-3c -- the Model / Skeleton switch (bottom left, above the status icon's slot) and
    // the skeleton overlay over the 3D image, in RAV view (the whole window) and Tagging view
    // (the area left of the panel, above the strip); hidden in Video view. After the panel, so
    // the overlay colours the rule selected this frame.
    SkeletonViewFrame(g_renderer);
    if (!video_view && !library_view) {
        const float view_w = tagging_view ? area_right : static_cast<float>(g_client_w);
        const float view_h = tagging_view ? ViewBottom() : static_cast<float>(g_client_h);
        DrawSkeletonOverlay(g_renderer, view_w, view_h, area_right - kNavCubeMargin - kNavCubeSize - ui::Dp(8.0f),
                            ViewBottom() - kNavCubeMargin);
        DrawSkeletonSwitch(kStatusIconMargin, ViewBottom() - kStatusIconMargin - kStatusIconSize - ui::Dp(6.0f));
    }

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
        // 13-14: the theme's floating card.
        const ImVec2 pos(area_right * 0.5f, video_view ? VideoTopBand() + ui::Dp(4.0f) : ui::Dp(48.0f));
        if (ui::BeginFloatingCard("##loadmsg", pos, ImVec2(0.5f, 0.0f))) {
            DrawLoadMessageBody();
            if (ImGui::IsWindowHovered() && g_load_msg_until < ElapsedSeconds() + 1.0)
                g_load_msg_until = ElapsedSeconds() + 1.0;
        }
        ui::EndFloatingCard();
    }

    // Story 6.5.8 — navigation cube (ViewCube), bottom-right. Drawn in THIS NewFrame/Render
    // pair (after the menu + read-outs, before Render) so it is the same single ImGui frame.
    // Story 11-4: in Video view it shows and edits the video camera, and is hidden when there
    // is no camera to edit (no active FX / no item).
    if (library_view) {
        // Epic 13 (D20): no 3D view in the Library view, so no nav cube.
    } else if (!video_view) {
        NavCubeWidget(g_renderer.Camera(), /*video=*/false, area_right, ViewBottom());
    } else {
        // Over the picture's bottom-right corner: the frame keeps no room for it.
        OrbitCamera video_cam;
        if (VideoViewCamera(&video_cam))
            NavCubeWidget(video_cam, /*video=*/true, g_video_frame.x + g_video_frame.w, g_video_frame.y + g_video_frame.h);
    }

    // Bottom-left status icon + its hover panel (re-shows the load message on demand).
    StatusIconWidget();

    // Story 6-5-10 -- the clip list: its round button on the left of every view, half-way between
    // the menu button (its bottom: 10 + 8 + 26 px) and the Model / Skeleton switch's slot (above the
    // strip in Tagging / Video view); open, the panel over the left of the view (never the Tagging /
    // Video panel). 6-5-10b: it covers the 3D view only: from under the menu button to above the
    // switch (RAV, Tagging) or the status icon (Video, Library: no switch there), so never the strip
    // or the timeline, whose top it follows when they are resized. Opening it closes the options menu.
    {
        const float icon_top = ViewBottom() - kStatusIconMargin - kStatusIconSize;
        const float switch_bottom = icon_top - ui::Dp(6.0f);
        const float switch_top = switch_bottom - ui::ControlH() - ui::Dp(4.0f);
        const bool  switch_shown = !video_view && !library_view;
        if (DrawClipList(area_right, ui::Dp(10.0f + 8.0f + 26.0f), switch_shown ? switch_top : icon_top, ViewBottom(),
                         ShortcutKeyLabel(kShortcutClipList)))
            g_menu_open = false;
    }

    // Spec viewer-options-menu-tabs: open, the menu is drawn above every other viewer window
    // (strip, panels, switches, nav cube), even one clicked this frame; popups, modals and
    // tooltips stay above it. EndFrame first: a click focuses (brings to front) its window there.
    ImGui::EndFrame();
    // Story 6-5-10: the clip list over every other viewer window (its button over its panel); the
    // menu, open, stays above both.
    if (ImGuiWindow* w = ClipListPanelWindow()) KeepMenuInFront(w);
    if (ImGuiWindow* w = ClipListButtonWindow()) KeepMenuInFront(w);
    if (g_menu_open) KeepMenuInFront(menu_window);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// 13-7: the UI fonts, those of the validated mock-up: --font Segoe UI (body 12 px, small
// 11, titles 13, big 15) and --mono Cascadia Mono / Consolas (numbers). ImGui's own
// ProggyClean (a 13 px pixel font, Basic Latin + Latin-1 only) stays the last resort.
//
// Sizes: CSS px are em sizes, but ImGui's size is the font's ascent + descent height
// (stbtt_ScaleForPixelHeight), and Segoe UI's ascent + descent is 1.33 em (hhea
// 2210 + 514 over 2048). So 12 CSS px = 16 ImGui px (a 16 px line, as a browser lays
// 12 px Segoe UI out), not 12: 12 would draw the letters a quarter smaller than the
// mock-up. Each file carries its own ratio below; all are scaled by the screen's DPI.
//
// Every font gets the Windows symbol fonts merged in for the blocks of
// ui_glyph_ranges.h (Segoe UI punctuation and arrows, then Segoe UI Symbol shapes,
// stars and dingbats, then Arial): the first font holding a code point wins, a font
// missing a glyph adds nothing, and a missing or unreadable file is skipped.
namespace {

struct UiFontFile {
    const wchar_t* name;
    float          height_per_em;   // (hhea ascent + descent) / units per em
};
constexpr UiFontFile kSegoeUi     = {L"segoeui.ttf", 1.330f};
constexpr UiFontFile kSegoeSymbol = {L"seguisym.ttf", 1.330f};
constexpr UiFontFile kArial       = {L"arial.ttf", 1.117f};
// 13-7: the 600 weight of the mockup's titles (Segoe UI Semibold shares Segoe UI's vertical
// metrics), Arial Bold when it is missing.
constexpr UiFontFile kSegoeSemibold = {L"seguisb.ttf", 1.330f};
constexpr UiFontFile kArialBold     = {L"arialbd.ttf", 1.117f};
constexpr UiFontFile kCascadia    = {L"CascadiaMono.ttf", 1.172f};
constexpr UiFontFile kConsolas    = {L"consola.ttf", 1.171f};

// The UTF-8 path of a file of the Windows Fonts folder, or "" when it is not there.
std::string WindowsFontPath(const std::wstring& fonts_dir, const wchar_t* file)
{
    const std::wstring path = fonts_dir + file;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string utf8(static_cast<size_t>(bytes), '\0');   // ImGui opens it as UTF-8 (_wfopen)
    if (WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &utf8[0], bytes, nullptr, nullptr) != bytes) return {};
    utf8.resize(static_cast<size_t>(bytes - 1));
    return utf8;
}

// Crisp small text: 2x horizontal oversampling for smooth subpixel placement, glyph
// advances snapped to whole pixels at UI sizes, and a touch more ink, as Segoe UI's
// thin stems at 11-13 px wash out grey under stb_truetype's plain coverage.
ImFontConfig UiFontConfig(float em_px)
{
    ImFontConfig cfg;
    cfg.OversampleH        = em_px >= 24.0f ? 1 : 2;   // large (high-DPI) sizes are smooth already; keeps the atlas small
    cfg.OversampleV        = 1;
    cfg.PixelSnapH         = em_px <= 16.0f;
    cfg.RasterizerMultiply = 1.15f;
    return cfg;
}

// Adds the first of `files` that loads at `em_px` (CSS px, DPI applied), the symbol
// fonts merged in. nullptr when none loaded.
ImFont* AddUiFont(ImGuiIO& io, const std::wstring& fonts_dir, std::initializer_list<UiFontFile> files, float em_px)
{
    ImFont* font = nullptr;
    for (const UiFontFile& f : files) {
        const std::string path = WindowsFontPath(fonts_dir, f.name);
        if (path.empty()) continue;
        ImFontConfig cfg = UiFontConfig(em_px);
        font = io.Fonts->AddFontFromFileTTF(path.c_str(), std::round(em_px * f.height_per_em), &cfg, kUiGlyphRanges);
        if (font) break;
    }
    if (!font) return nullptr;
    for (const UiFontFile& f : {kSegoeUi, kSegoeSymbol, kArial}) {
        const std::string path = WindowsFontPath(fonts_dir, f.name);
        if (path.empty()) continue;
        ImFontConfig cfg = UiFontConfig(em_px);
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(path.c_str(), std::round(em_px * f.height_per_em), &cfg, kUiGlyphRanges);
    }
    return font;
}

}  // namespace

// `dpi_scale` is the screen's DPI over 96 (1 at 100 % scaling).
void LoadUiFontAtlas(ImGuiIO& io, float dpi_scale)
{
    static_assert(std::is_same<ImWchar, std::remove_cv_t<std::remove_extent_t<decltype(kUiGlyphRanges)>>>::value,
                  "kUiGlyphRanges must match ImWchar (16-bit)");
    if (!(dpi_scale >= 1.0f && dpi_scale <= 4.0f)) dpi_scale = 1.0f;
    ui::SetFonts(ui::FontSet{});
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;   // high DPI: stay under GL_MAX_TEXTURE_SIZE

    wchar_t win_dir[MAX_PATH] = {};
    const UINT dir_len = GetWindowsDirectoryW(win_dir, MAX_PATH);
    if (dir_len == 0 || dir_len >= MAX_PATH) {
        io.Fonts->AddFontDefault();
        LogInfo("UI fonts: the Windows folder was not found; the built-in font is used");
        return;
    }
    const std::wstring fonts_dir = std::wstring(win_dir) + L"\\Fonts\\";

    // The body font first: the atlas's first font is ImGui's default.
    // 13-7 (audit A1): the mockup's whole type scale, one baked font per role (ImGui 1.91 has
    // no dynamic sizes): Segoe UI 12 (body, controls) / 11 (captions) / 13 (rows, section
    // titles) / 15 / 10 (units, tags) / 9 (badge); Segoe UI Semibold (600) 12 / 11 / 14 / 15
    // for bold names and titles; Cascadia Mono / Consolas 12 / 11 (number fields) / 10 / 9. A
    // role whose file is missing falls back to the nearest loaded one (ui_theme.cpp).
    ui::FontSet set;
    const auto sans = [&](float css) { return AddUiFont(io, fonts_dir, {kSegoeUi, kArial}, css * dpi_scale); };
    const auto bold = [&](float css) {
        return AddUiFont(io, fonts_dir, {kSegoeSemibold, kArialBold}, css * dpi_scale);
    };
    const auto mono = [&](float css) { return AddUiFont(io, fonts_dir, {kCascadia, kConsolas}, css * dpi_scale); };
    set.body = sans(12.0f);
    if (set.body) {
        set.small        = sans(11.0f);
        set.title        = sans(13.0f);
        set.big          = sans(15.0f);
        set.tiny         = sans(10.0f);
        set.badge        = sans(9.0f);
        set.semibold     = bold(12.0f);
        set.caption_bold = bold(11.0f);
        set.heading      = bold(14.0f);
        set.pane_title   = bold(15.0f);
        set.mono         = mono(12.0f);
        set.mono_small   = mono(11.0f);
        set.mono_tiny    = mono(10.0f);
        set.mono_micro   = mono(9.0f);
    }
    // Build now: a corrupt font file would fail the WHOLE atlas (no text at all), so fall
    // back to the built-in font alone rather than lose every label.
    if (!set.body || !io.Fonts->Build()) {
        io.Fonts->Clear();
        io.Fonts->AddFontDefault();
        LogInfo("UI fonts: Segoe UI / Arial could not be loaded; the built-in font is used");
        return;
    }
    io.FontDefault = set.body;
    ui::SetFonts(set);
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
    if (ImGuiContext* created = ImGui::CreateContext()) {
        g_main_imgui_ctx = created;
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // do not write imgui.ini next to Reaper
        io.LogFilename = nullptr;
        ui::ApplyRavTheme();   // Story 11-4: the DM-XYZ-Pad theme (ui_theme.h), over StyleColorsDark
        // 13-7: the mock-up's fonts (Segoe UI, Cascadia Mono / Consolas) at the screen's
        // DPI; the style's paddings and sizes follow the same scale.
        const int dpi_y = g_hdc ? GetDeviceCaps(g_hdc, LOGPIXELSY) : 96;
        // One scale for the style, the theme and the fonts, clamped to what the fonts accept.
        const float dpi_scale = std::clamp(dpi_y > 96 ? static_cast<float>(dpi_y) / 96.0f : 1.0f, 1.0f, 4.0f);
        if (dpi_scale > 1.0f) ImGui::GetStyle().ScaleAllSizes(dpi_scale);
        ui::SetUiScale(dpi_scale);   // the theme's widgets size themselves in mockup px x this (ui::Dp)
        LoadUiFontAtlas(io, dpi_scale);
        // Init the two backends in nested steps so teardown stays symmetric: if the GL
        // backend fails AFTER the Win32 backend already initialized, we must still shut the
        // Win32 backend down (DestroyContext alone leaks its platform data). [Review][Patch]
        if (ImGui_ImplWin32_Init(hwnd)) {
            if (ImGui_ImplOpenGL3_Init("#version 130")) {
                g_imgui_ready = true;
                SetBusyPainter(&PaintBusyCard, &DropBusyInputs);  // 13-59: long gestures show their card here
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
                g_icon_shadow = UploadIconTexture(kIcon_shadow, kIconSize, kIconSize);
                g_icon_ground = UploadIconTexture(kIcon_ground, kIconSize, kIconSize);
                g_icon_performance = UploadIconTexture(kIcon_performance, kIconSize, kIconSize);
                g_icon_keyboard = UploadIconTexture(kIcon_keyboard, kIconSize, kIconSize);
            } else {
                LogInfo("tool UI (Dear ImGui) could not initialize — the viewport still works");
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
                g_main_imgui_ctx = nullptr;
            }
        } else {
            LogInfo("tool UI (Dear ImGui) could not initialize — the viewport still works");
            ImGui::DestroyContext();
            g_main_imgui_ctx = nullptr;
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
        if (g_icon_shadow) { glDeleteTextures(1, &g_icon_shadow); g_icon_shadow = 0; }
        if (g_icon_ground) { glDeleteTextures(1, &g_icon_ground); g_icon_ground = 0; }
        if (g_icon_performance) { glDeleteTextures(1, &g_icon_performance); g_icon_performance = 0; }
        if (g_icon_keyboard) { glDeleteTextures(1, &g_icon_keyboard); g_icon_keyboard = 0; }
        if (g_imgui_ready) {
            SetBusyPainter(nullptr, nullptr);  // 13-59: no card without the viewer
            DestroyAssignFloat();  // 13-15: its context shares our font atlas and GL context: freed first
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            g_main_imgui_ctx = nullptr;
            g_imgui_ready = false;
        }
        ReleaseVideoTarget();  // Story 11-4: the Video view's target, while the context is current
        g_renderer.Shutdown();
    }
    DestroyGLContext();

    // Story 11-4: a gesture or queued write cannot outlive the panel (nothing is written).
    VideoViewOnViewerClosed();
    // Story 10-3: nor a Tagging view drag (its preview is dropped); its model is read afresh.
    ui::DragNumberReset();
    TaggingSessionReset();
    g_drag_video = false;
    g_video_post_pending = false;
    g_tagging_dialog_pending = false;  // story 10-3b: a post lost with the window
    g_populate_post_pending = false;   // 13-19: so is a queued Populate (nothing written)
    DropPendingPopulate();
    g_current_load_ok = false;

    // The renderer's asset died with the context, so forget which item it showed: a
    // reopened panel must reload the item under the playhead, not skip it as "already
    // loaded" (the reload gate compares against this path) and draw nothing.
    g_current_anim_path.clear();
    g_transport_driven = false;
    g_view_cleared     = false;
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
// Story 10-3b: the Tagging view's preset menu open counts as a text field (it takes every key).
// Story 10-3d: so does the Skeleton & roles window (its Ctrl+Z is its own undo, not REAPER's).
bool TextInputNow()
{
    return (g_imgui_ready && ImGui::GetIO().WantTextInput) ||
           (TaggingViewActive() && (TaggingPresetMenuOpen() || TaggingRolesWindowOpen()));
}
bool ViewerTakesCharNow() { return ViewerTakesChar(g_key_route, ShortcutRecordingId() >= 0, TextInputNow()); }

// 13-19 / 13-59: one frame of the busy card of a long gesture (busy_progress_ui.h) on RAV's window:
// the theme background and a centred card (title, bar, "Item i of n · event j of m", note). Only
// this card is drawn: the panels would read the project while the gesture is half written. 13-59:
// the gesture may run inside an ImGui frame (a button of a panel, the assignment window's frame) or
// outside any (Populate from the window procedure, the follow from a timer), so the card is built
// as a draw list of its own and rendered by the main context's GL backend: no NewFrame, nothing of a
// frame in progress is touched. The GL state it changes (framebuffer, viewport, scissor, clear colour)
// and the GL / ImGui contexts in place are put back. Then the thread peeks its queue without taking
// anything (PM_NOREMOVE), so Windows does not replace REAPER by a "Not responding" ghost after 5 s
// and hide the progress; no input is read, nothing is dispatched.
bool PaintBusyCard(const BusyFrame& f)
{
    bool painted = false;  // 13-59 review: said back, so a card never painted changes nothing after the gesture
    if (g_imgui_ready && g_main_imgui_ctx && g_hwnd && g_hdc && g_hglrc && IsWindowVisible(g_hwnd) &&
        g_client_w > 0 && g_client_h > 0) {
        const HGLRC prev_rc = wglGetCurrentContext();
        const HDC   prev_dc = wglGetCurrentDC();
        const bool  switched = prev_rc != g_hglrc || prev_dc != g_hdc;
        if (!switched || wglMakeCurrent(g_hdc, g_hglrc)) {
            ImGuiContext* prev_ctx = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(g_main_imgui_ctx);
            ImGuiIO& io = ImGui::GetIO();
            ImFont*  font = io.FontDefault ? io.FontDefault : (io.Fonts && !io.Fonts->Fonts.empty() ? io.Fonts->Fonts[0] : nullptr);
            if (font && io.Fonts && ImGui::GetFrameCount() > 0) {  // a first frame built the font and its texture
                constexpr GLenum kFramebufferBinding = 0x8CA6;  // GL_FRAMEBUFFER_BINDING
                GLint            prev_fb = 0, prev_vp[4] = {0, 0, 0, 0};
                GLfloat          prev_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                glGetIntegerv(kFramebufferBinding, &prev_fb);
                glGetIntegerv(GL_VIEWPORT, prev_vp);
                glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear);
                const GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, g_client_w, g_client_h);
                glDisable(GL_SCISSOR_TEST);
                const ImVec4 bg = ui::Col(ui::kBg);
                glClearColor(bg.x, bg.y, bg.z, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                const float W = static_cast<float>(g_client_w), H = static_cast<float>(g_client_h);
                const float fs = font->FontSize;
                const float pad_x = ui::Dp(16.0f), pad_y = ui::Dp(14.0f), gap = ui::Dp(8.0f), bar_h = ui::Dp(8.0f);
                const float card_w = std::min(ui::Dp(360.0f), std::max(ui::Dp(120.0f), W - ui::Dp(32.0f)));
                const float text_w = std::max(1.0f, card_w - 2.0f * pad_x);
                const char* kNote = "REAPER resumes when it is done. One undo point.";
                const std::string status = BusyText(f);
                const float title_h = font->CalcTextSizeA(fs, FLT_MAX, text_w, f.title.c_str()).y;
                const float status_h = font->CalcTextSizeA(fs, FLT_MAX, text_w, status.c_str()).y;
                const float note_h = font->CalcTextSizeA(fs, FLT_MAX, text_w, kNote).y;
                const float card_h = 2.0f * pad_y + title_h + gap + bar_h + gap + status_h + ui::Dp(4.0f) + note_h;
                const ImVec2 c0(std::floor((W - card_w) * 0.5f), std::floor(std::max(0.0f, (H - card_h) * 0.5f)));
                const ImVec2 c1(c0.x + card_w, c0.y + card_h);

                ImDrawList dl(ImGui::GetDrawListSharedData());
                dl._ResetForNewFrame();
                dl.PushTextureID(io.Fonts->TexID);
                dl.PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(W, H));
                dl.AddRectFilled(c0, c1, ui::kSurface, ui::Dp(10.0f));
                dl.AddRect(c0, c1, ui::kStroke, ui::Dp(10.0f));
                float y = c0.y + pad_y;
                dl.AddText(font, fs, ImVec2(c0.x + pad_x, y), ui::kText, f.title.c_str(), nullptr, text_w);
                y += title_h + gap;
                const ImVec2 b0(c0.x + pad_x, y), b1(c1.x - pad_x, y + bar_h);
                dl.AddRectFilled(b0, b1, ui::kBg, ui::Dp(4.0f));
                const float fill = BusyFraction(f);
                if (fill > 0.0f) dl.AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * fill, b1.y), ui::kAccent, ui::Dp(4.0f));
                y += bar_h + gap;
                dl.AddText(font, fs, ImVec2(c0.x + pad_x, y), ui::kMuted, status.c_str(), nullptr, text_w);
                y += status_h + ui::Dp(4.0f);
                dl.AddText(font, fs, ImVec2(c0.x + pad_x, y), ui::kFaint, kNote, nullptr, text_w);

                ImDrawData dd;
                dd.Valid = true;
                dd.DisplayPos = ImVec2(0.0f, 0.0f);
                dd.DisplaySize = ImVec2(W, H);
                dd.FramebufferScale = ImVec2(1.0f, 1.0f);
                dd.AddDrawList(&dl);
                ImGui_ImplOpenGL3_RenderDrawData(&dd);
                SwapBuffers(g_hdc);
                painted = true;

                glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fb));
                glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
                glClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]);
                if (prev_scissor) glEnable(GL_SCISSOR_TEST);
            }
            ImGui::SetCurrentContext(prev_ctx);
            if (switched) wglMakeCurrent(prev_dc, prev_rc);
        }
    }
    MSG peek;
    PeekMessageW(&peek, nullptr, 0, 0, PM_NOREMOVE);
    return painted;
}

// 13-19 review / 13-59: the clicks, wheel turns and keys made on the busy card are dropped once the
// gesture is over, so they reach neither the widgets below nor a second run of the gesture. Only the
// presses: a release is let through (a button pressed before the gesture must not stay held). Review:
// on RAV's window and on the Sound assignment window (a gesture run from its frame, clicked again).
void DropBusyInputs()
{
    static const UINT kDropped[] = {WM_LBUTTONDOWN, WM_LBUTTONDBLCLK, WM_RBUTTONDOWN, WM_RBUTTONDBLCLK,
                                    WM_MBUTTONDOWN, WM_MBUTTONDBLCLK, WM_XBUTTONDOWN, WM_XBUTTONDBLCLK,
                                    WM_MOUSEWHEEL,  WM_MOUSEHWHEEL,   WM_KEYDOWN,     WM_SYSKEYDOWN,
                                    WM_CHAR,        WM_SYSCHAR};
    const HWND targets[] = {g_hwnd, AssignFloatHwnd()};
    MSG        m;
    for (HWND h : targets) {
        if (!h || !IsWindow(h)) continue;
        for (UINT msg : kDropped)
            while (PeekMessageW(&m, h, msg, msg, PM_REMOVE)) {}
    }
}

// Story 6-5-9 -- full screen on / off: the same window leaves the docker or goes back to it
// (src/viewer_fullscreen.h). A running camera / Video view drag ends first (a Video view drag is
// written), as when the view changes. Never called inside the render tick.
void SetFullScreenMode(bool on)
{
    if (!g_hwnd || !IsWindow(g_hwnd) || on == FullScreenActive()) return;
    if (g_drag != DragMode::None) {
        if (g_drag_video) VideoViewEndDrag();
        g_drag_video = false;
        g_drag = DragMode::None;
        ReleaseCapture();
    }
    if (on) EnterFullScreen(g_hwnd, g_reaper_main_hwnd, kDockName, kDockIdent);
    else    LeaveFullScreen(g_hwnd, g_reaper_main_hwnd, kDockName, kDockIdent);
    if (g_full_screen_cmd) RefreshToolbar(g_full_screen_cmd);
}

// What a KEYS row does (src/shortcuts.h), from its key in the viewer or (13-60b) from its action in
// REAPER's list (RunViewerShortcut). The caller checked the row's context.
void RunShortcutAction(int action)
{
    if (action == kShortcutFullScreen) {  // story 6-5-9 (F11)
        SetFullScreenMode(!FullScreenActive());
        return;
    }
    if (action == kShortcutClipList) {  // story 6-5-10 (L): the clip list, every view
        SetClipListOpen(!ClipListOpen(), "its shortcut");
        if (ClipListOpen()) g_menu_open = false;
        return;
    }
    if (action == kShortcutToggleView || (action == kShortcutTogglePanel && !VideoViewActive())) {
        // A running drag ends before the view changes (a Video view drag is written).
        // The panel key from RAV view opens the panel, which enters Video view: same rule.
        if (g_drag != DragMode::None) {
            if (g_drag_video) VideoViewEndDrag();
            g_drag_video = false;
            g_drag = DragMode::None;
            ReleaseCapture();
        }
        // Story 10-3: V cycles RAV view -> Tagging view -> Video view -> RAV view. A Tagging
        // view drag ends as shown (RenderTick sees the view change).
        if (action == kShortcutToggleView) SetViewMode(NextViewMode(GetViewMode()));
        else                               SetVideoPanelVisible(true);
        return;
    }
    if (action == kShortcutTogglePanel) { SetVideoPanelVisible(!VideoPanelVisible()); return; }
    // Story 11-5 -- cut a new shot at the playhead (Video view only: the table's context).
    if (action == kShortcutCut) { QueueVideoCut(); return; }
    // Spec 11-fb-11 -- delete the current shot (Video view only), after a confirmation.
    if (action == kShortcutDeleteShot) { RequestVideoDeleteCurrentShot(); return; }
    // The shot camera clipboard, on the current shot (Video view only).
    if (action == kShortcutCopyCamera) { QueueVideoCopyShotCamera(-1); return; }
    if (action == kShortcutPasteCamera) { QueueVideoPasteShotCamera(-1); return; }
    // Story 10-4 -- Tagging view: E adds an event at the playhead on the selected rule, Del
    // suppresses / restores / deletes the selected event (one undo point each).
    if (action == kShortcutTagAddEvent) { TaggingAddEventAtPlayhead(); return; }
    if (action == kShortcutTagDelEvent) { TaggingDeleteSelectedEvent(); return; }
    // 13-61 -- the Sound Edit window's trims (Library Edit only): done by its next frame.
    if (action == kShortcutEditTrimLeft || action == kShortcutEditTrimRight || action == kShortcutEditTrimZone) {
        RequestLibraryEditTrim(action);
        return;
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Only when another context is current (then put back): WM_CREATE makes ours, WM_DESTROY frees it.
    struct MainContext {
        ImGuiContext* prev = nullptr;
        bool          switched = false;
        MainContext()
        {
            ImGuiContext* cur = ImGui::GetCurrentContext();
            if (g_main_imgui_ctx && cur != g_main_imgui_ctx) {
                prev = cur;
                ImGui::SetCurrentContext(g_main_imgui_ctx);
                switched = true;
            }
        }
        ~MainContext()
        {
            if (switched) ImGui::SetCurrentContext(prev);
        }
    } main_context;
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
    // 13-45: a "View: scroll and zoom" row of the Shortcuts popup records the wheel; the notch is the
    // recording's only (not fed to Dear ImGui: the popup does not scroll under it).
    if ((msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) && ViewNavRecordingRow() >= 0) {
        ViewNavRecordWheel(msg == WM_MOUSEHWHEEL, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_MENU) < 0);
        return 0;
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

    case WM_MOUSEHWHEEL:
        // 13-45: the horizontal wheel (a tilt wheel, a trackpad's sideways swipe). ImGui has it
        // already (io.MouseWheelH): over the UI it is the timelines' (view_nav.h), and it must not
        // reach DefWindowProc, which hands it to the parent (REAPER would scroll its arrange view).
        // Over the 3D view it does nothing. (A Shortcuts row being recorded took it above.)
        if (ImGuiWantsMouse() && GetKeyState(VK_MENU) < 0) g_key_route.alt_wheel_taken = true;
        return 0;

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
        // Story 10-3: Esc during a Tagging view drag cancels it (ImGui has the key already).
        if (wp == VK_ESCAPE && TaggingViewActive() && TaggingGestureActive()) return 0;
        // Story 6-5-10: the clip list has the keyboard: its arrows, Enter and Esc are read by its
        // frame (ImGui has the key already).
        if (ClipListTakesKey(static_cast<unsigned>(wp), ctrl, shift, alt)) return 0;
        // ...open without it (the 3D view was clicked since): Esc closes it, before leaving full screen.
        // One Esc, one thing: an open ImGui menu / combo / popup (its state at key-down: ImGui reads
        // the key at the next frame) or the Library's Edit overlay takes this press; the list closes
        // at the next one (full screen stays: es.clip_list below).
        if (wp == VK_ESCAPE && ClipListOpen() && !TextInputNow()) {
            const bool popup = ImGui::GetCurrentContext() &&
                               ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            const bool library = LibraryViewActive() && LibraryEditUsesEsc();
            if (!popup && !library) {
                if (!(lp & (1 << 30))) SetClipListOpen(false, "Esc in the viewer");
                return 0;
            }
        }
        // A text field, or the Tagging view's preset menu (story 10-3b), takes every key.
        if (TextInputNow()) return 0;
        // 13-15b: no viewer shortcut (Del, E, V...) runs while the Sound assignment window has the focus;
        // the keys it takes are its own, the others go to REAPER (the accelerator hook).
        if (TaggingViewActive() && TaggingAssignWindowOpen()) return 0;
        // Story 6-5-9: full screen, an Esc that nothing else takes leaves it (the Library view keeps its own).
        if (wp == VK_ESCAPE && FullScreenActive()) {
            fullscreen::EscState es;
            es.full_screen = true;
            es.shortcuts_popup = ShortcutsPopupOpen();
            es.delete_confirm = VideoDeleteConfirmOpen();
            es.tagging_gesture = TaggingViewActive() && TaggingGestureActive();
            es.text_input = TextInputNow();
            es.recording = ShortcutRecordingId() >= 0;
            es.clip_list = ClipListOpen();  // story 6-5-10 (closed above already; kept for the rule)
            es.library_takes_esc = LibraryViewActive() && LibraryViewTakesKey(VK_ESCAPE, ctrl, shift, alt);
            // Under the main ImGui context (MainContext above); ImGui reads this key only at the next
            // frame, so this is the popup state at key-down: an open menu / combo closes, full screen stays.
            es.imgui_popup = ImGui::GetCurrentContext() &&
                             ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            if (!fullscreen::EscLeavesFullScreen(es)) break;
            if (!(lp & (1 << 30))) SetFullScreenMode(false);
            return 0;
        }
        // 13-60 review: a key the focused Library view (or its Edit) takes is its own: a RAV shortcut on
        // the same key (Space linked to an action REAPER gives Space) does not run as well.
        const bool library_key =
            LibraryViewActive() && LibraryViewTakesKey(static_cast<unsigned>(wp), ctrl, shift, alt);
        const int action = library_key ? -1
                                       : ShortcutActionForKey(static_cast<unsigned>(wp), ctrl, shift, alt,
                                                              VideoViewActive(), TaggingViewActive(),
                                                              LibraryViewActive() && LibraryEditKeysActive());  // 13-61
        if (action < 0) {
            // The auto-repeat of a key the hook took (it may map to no action now): ours.
            if ((lp & (1 << 30)) && g_key_route.claimed_vk != 0 && wp == g_key_route.claimed_vk) return 0;
            break;  // not ours (an Alt key goes on to DefWindowProc)
        }
        if (lp & (1 << 30)) return 0;  // auto-repeat of a held key: one toggle per press
        if (VideoDeleteConfirmOpen()) return 0;  // spec 11-fb-11: no shortcut runs behind the confirmation
        RunShortcutAction(action);  // 13-60b: shared with REAPER's own actions for these keys
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
                                VideoDeleteConfirmOpen() || g_key_route.claimed_vk != 0 || ClipListOpen();
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

    case kMsgRunPopulate: {
        // 13-19: the queued Populate / Restore (one undo point), its progress drawn about every 100 ms.
        g_populate_post_pending = false;
        if (g_populate_running) return 0;  // 13-19 review: a nested run is ignored
        try {
            struct RunningGuard {
                RunningGuard() { g_populate_running = true; }
                ~RunningGuard() { g_populate_running = false; }
            } running;
            // 13-59: the shared busy card (shown after ~150 ms); its end drops the clicks / keys made
            // on it (DropBusyInputs), so they reach neither the widgets below nor a second Populate.
            BusyScope busy("Populating sounds");
            RunPendingPopulate([&busy](const PopulateProgress& p) {
                busy.Item(p.item, p.items);
                BusyStep(p.event, p.events);
            });
        } catch (const std::exception& e) {
            LogError("Populate failed: %s", e.what());
        } catch (...) {
            LogError("Populate failed");
        }
        return 0;
    }

    case kMsgTaggingDialog: {
        // Story 10-3b -- the preset Import / Export picker, owned by the top-level window (the
        // viewer may be a docked child of REAPER's docker).
        g_tagging_dialog_pending = false;
        HWND owner = g_hwnd ? GetAncestor(g_hwnd, GA_ROOT) : nullptr;
        if (!owner) owner = g_hwnd;
        try {
            TaggingRunPendingDialog(owner);
        } catch (const std::exception& e) {
            LogError("Preset import / export failed: %s", e.what());
        } catch (...) {
            LogError("Preset import / export failed");
        }
        return 0;
    }

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

    case kMsgFullScreen:
        // Story 6-5-9 -- the Full screen button's click, here outside the render tick.
        g_full_screen_post_pending = false;
        SetFullScreenMode(!FullScreenActive());
        return 0;

    case WM_SYSCOMMAND:
        // Story 6-5-9 -- full screen, the window is top-level: Alt+F4 (SC_CLOSE) leaves full
        // screen instead of destroying it. Docked, nothing changes.
        if ((wp & 0xFFF0) == SC_CLOSE && FullScreenActive()) {
            SetFullScreenMode(false);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (FullScreenActive()) {  // story 6-5-9: the same, for a WM_CLOSE sent by anything else
            SetFullScreenMode(false);
            return 0;
        }
        break;

    case WM_MOUSEACTIVATE:
        // Story 6-5-9 -- full screen, a click while a REAPER modal window is open (its main window
        // disabled) brings that window to the front instead of hiding it behind the viewer.
        if (FullScreenActive() && FullScreenForwardToReaperModal(hwnd, g_reaper_main_hwnd)) return MA_NOACTIVATEANDEAT;
        break;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        // Story 6-5-9 -- full screen, the monitor's size or scale changed: cover it again (the
        // suggested rect of WM_DPICHANGED is a window's, not the monitor's). Docked, the docker lays
        // the window out; the UI scale stays the one it opened with, as when docked.
        if (FullScreenActive()) {
            RefitFullScreen(hwnd);
            return 0;
        }
        break;

    case WM_SETTINGCHANGE:
        // Story 6-5-10 -- full screen, the work area changed (the taskbar moved, resized, or was set
        // to hide itself or not): fit it again. Every broadcast, not only SPI_SETWORKAREA: an
        // auto-hide taskbar moved to another edge leaves the work area as it was and Windows names
        // no SPI for it. RefitFullScreen does nothing when the window is already where it goes.
        // Every other setting goes on as before.
        if (FullScreenActive()) RefitFullScreen(hwnd);
        break;

    case WM_DESTROY:
        // Reached only on the paths WE initiate (CloseViewerWindow → DestroyWindow,
        // and unload) — NOT the docker tab X, which merely hides us (see header). Tear
        // the render loop + GL (and ImGui) down here while the context is still current.
        // Story 6-5-9: also when REAPER destroys the full screen window's owner (REAPER quits).
        ForgetFullScreen();
        g_full_screen_post_pending = false;  // a click posted to this window dies with it
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
    // 13-15: the Sound assignment window (a window of its own): typing in a field, and its own keys (Esc, the
    // arrows, Enter, Tab, Delete / Backspace) go to it; every other key (Space, Ctrl+Z...) stays REAPER's.
    if (AssignFloatOwns(msg->hwnd)) {
        const bool key_msg = msg->message == WM_KEYDOWN || msg->message == WM_KEYUP || msg->message == WM_CHAR ||
                             msg->message == WM_SYSKEYDOWN || msg->message == WM_SYSKEYUP || msg->message == WM_SYSCHAR;
        if (!key_msg) return 0;
        if (msg->message == WM_SYSKEYDOWN && msg->wParam == VK_F4) return kRouteViewerSys;  // Alt+F4 closes it
        if (AssignFloatWantsText()) return msg->message == WM_SYSKEYDOWN || msg->message == WM_SYSKEYUP || msg->message == WM_SYSCHAR
                                               ? 0
                                               : -1;
        const bool     ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
        const unsigned vk = static_cast<unsigned>(msg->wParam);
        const bool own = vk == VK_ESCAPE || vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_RETURN ||
                         vk == VK_TAB || vk == VK_DELETE || vk == VK_BACK;
        if ((msg->message == WM_KEYDOWN || msg->message == WM_KEYUP) && !ctrl && !alt && own) return -1;
        return 0;
    }
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
    case WM_MOUSEHWHEEL: in.msg = KeyMsg::Wheel; break;  // 13-45: the horizontal wheel too
    default:             in.msg = KeyMsg::Other; break;
    }
    in.key        = static_cast<unsigned>(msg->wParam);
    in.repeat     = (msg->lParam & (1 << 30)) != 0;
    in.ctrl       = GetKeyState(VK_CONTROL) < 0;
    in.shift      = GetKeyState(VK_SHIFT) < 0;
    in.alt        = GetKeyState(VK_MENU) < 0;
    in.video_view = VideoViewActive();
    in.tagging_view = TaggingViewActive();  // Story 10-4: E / Del
    in.library_edit = LibraryViewActive() && LibraryEditKeysActive();  // 13-61: the Sound Edit window's trims
    in.recording  = ShortcutRecordingId() >= 0;
    in.text_input = TextInputNow();  // story 10-3b: the preset menu open counts
    // Esc closes either; Story 10-3: Esc also cancels a Tagging view drag.
    in.popup_open = ShortcutsPopupOpen() || VideoDeleteConfirmOpen() || (TaggingViewActive() && TaggingGestureActive()) ||
                    ClipListOpen();  // story 6-5-10: Esc closes the clip list
    in.confirm_open = VideoDeleteConfirmOpen();  // Enter confirms the delete
    in.imgui_mouse = ImGuiWantsMouse();          // spec 11-fb-12: Alt + wheel over the UI
    in.full_screen = FullScreenActive();         // story 6-5-9: Esc leaves, other keys reach REAPER
    in.horizontal = msg->message == WM_MOUSEHWHEEL;
    if (in.msg == KeyMsg::Wheel) in.view_wheel = ViewNavClaimsWheel(in.horizontal, in.ctrl, in.shift, in.alt);  // 13-45
    // Story 6-5-10: the clip list's own keys (Up, Down, Enter, Esc) while it has the keyboard, before the
    // Library view's (the list is over it); the key's release follows it.
    if (in.msg == KeyMsg::KeyDown && ClipListTakesKey(in.key, in.ctrl, in.shift, in.alt)) {
        in.text_input = true;
        g_key_route.claimed_vk = in.key;
    }
    // Story 13-7a (decision 7): the Library view's keys (Delete, F2, Space, Esc, Ctrl+B / D / X / V)
    // reach the viewer while its window has the focus; the key's character and release follow it.
    if (in.msg == KeyMsg::KeyDown && LibraryViewActive() && LibraryViewTakesKey(in.key, in.ctrl, in.shift, in.alt)) {
        in.text_input = true;
        g_key_route.claimed_vk = in.key;
    }
    // 13-15b: the same for the Tagging view's Sound assignment window while it has the focus.
    if (in.msg == KeyMsg::KeyDown && TaggingViewActive() && TaggingAssignTakesKey(in.key, in.ctrl, in.shift, in.alt)) {
        in.text_input = true;
        g_key_route.claimed_vk = in.key;
    }
    // 13-45: the view zoom / scroll keys (view_nav.h: REAPER's Zoom in / out keys, or the user's own)
    // while a timeline (the Video / Tagging strip, a Blend's lanes) is under the mouse.
    if (in.msg == KeyMsg::KeyDown && ViewNavTakesKey(in.key, in.ctrl, in.shift, in.alt)) {
        in.text_input = true;
        g_key_route.claimed_vk = in.key;
    }
    // The decision itself is pure (src/shortcuts.h RouteViewerKey, host-tested).
    return RouteViewerKey(CurrentShortcutKeymap(), in, g_key_route);  // 13-60: linked actions' keys too
}

accelerator_register_t g_accel_reg = { &ViewerTranslateAccel, true, nullptr };

}  // namespace

accelerator_register_t* ViewerAcceleratorRegistration()
{
    return &g_accel_reg;
}

void OpenViewerWindow(REAPER_PLUGIN_HINSTANCE hInst, HWND reaper_main)
{
    g_reaper_main_hwnd = reaper_main;  // story 6-5-9: the docked child's parent, the full screen owner
    if (g_hwnd && IsWindow(g_hwnd)) {
        // Story 6-5-9: full screen it is in no docker; it just comes to the front.
        if (FullScreenActive()) {
            SetForegroundWindow(g_hwnd);
            return;
        }
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
    const bool was_full_screen = FullScreenActive();
    if (g_hwnd && IsWindow(g_hwnd)) {
        // This is a path WE own (explicit close / unload), so detach from the
        // docker first (AR15), then destroy — WM_DESTROY runs StopRendering().
        // Story 6-5-9: full screen it left the docker already (no second DockWindowRemove);
        // its tool windows go back to REAPER's main window first. The next open is docked.
        if (was_full_screen) ForgetFullScreen();
        else                 DockWindowRemove(g_hwnd);
        DestroyWindow(g_hwnd);
    } else {
        ForgetFullScreen();
        StopRendering();         // window already hidden/gone; make sure the timer/context are down
    }
    g_hwnd = nullptr;
    if (was_full_screen && g_full_screen_cmd) RefreshToolbar(g_full_screen_cmd);

    // Unregister the window class so a future DLL load doesn't inherit a stale
    // lpfnWndProc pointing into our unmapped image.
    UnregisterViewerClass();
}

void ToggleViewerFullScreen(REAPER_PLUGIN_HINSTANCE hInst, HWND reaper_main)
{
    // AR18 -- the same no-throw boundary as ToggleViewerWindow (called from REAPER's hookcommand).
    try {
        if (FullScreenActive()) {
            SetFullScreenMode(false);
            return;
        }
        // Closed (or hidden by a docker X): open it first, docked, then straight to full screen.
        OpenViewerWindow(hInst, reaper_main);
        if (g_hwnd && IsWindow(g_hwnd)) SetFullScreenMode(true);
    } catch (const std::exception& e) {
        LogError("viewer full screen failed: %s (Reaper is unaffected)", e.what());
    } catch (...) {
        LogError("viewer full screen failed: unknown error (Reaper is unaffected)");
    }
}

bool ViewerIsFullScreen()
{
    return FullScreenActive() && g_hwnd && IsWindow(g_hwnd);
}

void SetViewerFullScreenCommandId(int command_id)
{
    g_full_screen_cmd = command_id;
}

void RunViewerShortcut(int id)
{
    // 13-60b: REAPER runs a KEYS row's action (its key in REAPER, a toolbar, the Actions list). The viewer
    // does it when it is open and the row's view is shown, as its key would; nothing behind a confirmation,
    // a recording, a text field or the Sound assignment window. Under the viewer's ImGui context.
    if (id < 0 || id >= kShortcutCount || !ViewerWindowIsVisible()) return;
    if (kShortcutTable[id].context == ShortcutContext::VideoView && !VideoViewActive()) return;
    if (kShortcutTable[id].context == ShortcutContext::TaggingView && !TaggingViewActive()) return;
    if (kShortcutTable[id].context == ShortcutContext::LibraryEdit && !(LibraryViewActive() && LibraryEditKeysActive())) return;
    if (VideoDeleteConfirmOpen() || ShortcutRecordingId() >= 0) return;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    if (g_main_imgui_ctx) ImGui::SetCurrentContext(g_main_imgui_ctx);
    const bool busy = TextInputNow() || (TaggingViewActive() && TaggingAssignWindowOpen());
    try {
        if (!busy) RunShortcutAction(id);
    } catch (...) {  // AR18: never into REAPER's message pump
        LogError("viewer shortcut %s failed (Reaper is unaffected)", kShortcutTable[id].id);
    }
    if (g_main_imgui_ctx) ImGui::SetCurrentContext(prev);
}

}  // namespace rav
