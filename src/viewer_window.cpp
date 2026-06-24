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

#include "console_log.h"
#include "gl_loader.h"   // LoadGlFunctions + modern-GL pointers; pulls in <gl/GL.h>
#include "renderer.h"

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

UINT_PTR g_timer_id = 0;
int g_client_w = kInitialWidth;
int g_client_h = kInitialHeight;

// Frame clock + present-rate fps (QueryPerformanceCounter; the timer cadence is
// only the lower bound — fps is measured from actual SwapBuffers calls).
LARGE_INTEGER g_qpc_freq{};
LARGE_INTEGER g_qpc_start{};
LARGE_INTEGER g_fps_last{};
int           g_frame_count = 0;

// True while the panel is hidden and the render loop is parked — see FrameTimerProc.
bool g_render_paused = false;

double ElapsedSeconds()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - g_qpc_start.QuadPart) / double(g_qpc_freq.QuadPart);
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

void RenderTick()
{
    // The context was made current once in StartRendering and stays current on
    // this (single render) thread, so no per-frame wglMakeCurrent is needed.
    if (!g_hdc || !g_hglrc) return;

    g_renderer.RenderFrame(static_cast<float>(ElapsedSeconds()), g_client_w, g_client_h);
    SwapBuffers(g_hdc);

    ++g_frame_count;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double elapsed = double(now.QuadPart - g_fps_last.QuadPart) / double(g_qpc_freq.QuadPart);
    if (elapsed >= 1.0) {
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
            LogInfo("panel hidden — render paused");
        }
        return;
    }
    if (g_render_paused) {
        g_render_paused = false;
        QueryPerformanceCounter(&g_fps_last);  // don't skew the first second after the pause
        g_frame_count = 0;
        LogInfo("panel shown — render resumed");
    }
    RenderTick();
}

// Brings up the GL context + renderer + frame loop. Returns false (with a
// console diagnostic) on any failure so WM_CREATE can bail without a broken
// window. (Story 1.4 hardens this into the full symmetric teardown-on-bail path.)
bool StartRendering(HWND hwnd)
{
    if (!CreateGLContextFor(hwnd)) {
        LogError("failed to create WGL context");
        return false;
    }
    if (!wglMakeCurrent(g_hdc, g_hglrc)) {
        LogError("wglMakeCurrent failed");
        return false;
    }

    std::string err;
    if (!LoadGlFunctions(err)) {
        LogError("%s", err.c_str());
        return false;
    }
    if (!g_renderer.Init(err)) {
        LogError("renderer init failed: %s", err.c_str());
        return false;
    }

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
        LogError("SetTimer failed");
        return false;
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
        g_renderer.Shutdown();
    }
    DestroyGLContext();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        if (!StartRendering(hwnd)) {
            StopRendering();   // undo whatever partially came up; no broken window
            return -1;         // abort window creation
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
        return 0;

    case WM_ERASEBKGND:
        return 1;  // GL owns the surface; skip GDI background fill to avoid flicker.

    case WM_DESTROY:
        // Reached only on the paths WE initiate (CloseViewerWindow → DestroyWindow,
        // and unload) — NOT the docker tab X, which merely hides us (see header).
        // Tear the render loop + GL down here while the context is still current.
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

}  // namespace

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
        // WM_CREATE returned -1 (GL/renderer init failed) or the create failed
        // outright; StartRendering already logged the cause.
        LogError("CreateWindowExW failed");
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
    // A visible panel toggles off (full close); a missing or docker-hidden panel
    // toggles on. OpenViewerWindow raises an already-created-but-hidden panel via
    // DockWindowActivate, or creates + docks a fresh one.
    if (ViewerWindowIsVisible()) {
        CloseViewerWindow();
    } else {
        OpenViewerWindow(hInst, reaper_main);
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

    // Unregister the window class so a future DLL load doesn't inherit a
    // stale lpfnWndProc pointing into our unmapped image.
    if (g_class_registered) {
        UnregisterClassW(kWindowClassName, g_class_hinst);
        g_class_registered = false;
        g_class_hinst      = nullptr;
    }
}

}  // namespace rav
