// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.

#include "spike_glwindow.h"

#include <cstdio>

#include "spike_gl.h"        // modern-GL pointers + constants + GlLoadFunctions
#include "spike_loader.h"
#include "spike_renderer.h"
#include "reaper_api.h"      // ShowConsoleMsg

namespace spike {
namespace {

constexpr wchar_t kClass[] = L"FBXAV.Spike.GlDockWnd";

HWND      g_hwnd = nullptr;
HDC       g_dc   = nullptr;
HGLRC     g_glrc = nullptr;
HINSTANCE g_hinst = nullptr;
bool      g_class_registered = false;
int       g_w = 1, g_h = 1;

Renderer  g_renderer;

LARGE_INTEGER g_freq{};
LARGE_INTEGER g_lastFpsCount{};
int    g_frames = 0;
double g_fps = 0.0;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        g_w = LOWORD(lp);
        g_h = HIWORD(lp);
        if (g_w < 1) g_w = 1;
        if (g_h < 1) g_h = 1;
        return 0;
    case WM_ERASEBKGND:
        return 1;  // GL owns the surface; skip GDI erase to avoid flicker
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);  // the timer drives rendering; just validate the region
        return 0;
    }
    case WM_DESTROY:
        g_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND GlWindowCreate(HINSTANCE hinst, HWND parent, const std::string& fixturePath,
                    std::string& outError)
{
    g_hinst = hinst;

    if (!g_class_registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_OWNDC;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hinst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                outError = "RegisterClassExW failed";
                return nullptr;
            }
        }
        g_class_registered = true;
    }

    g_hwnd = CreateWindowExW(0, kClass, L"FBXAV Spike (GL docked)",
                             WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                             0, 0, 600, 400, parent, nullptr, hinst, nullptr);
    if (!g_hwnd) { outError = "CreateWindowExW failed"; return nullptr; }

    g_dc = GetDC(g_hwnd);
    if (!g_dc) { outError = "GetDC failed"; GlWindowDestroy(); return nullptr; }

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.cDepthBits   = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType   = PFD_MAIN_PLANE;
    const int pf = ChoosePixelFormat(g_dc, &pfd);
    if (!pf || !SetPixelFormat(g_dc, pf, &pfd)) {
        outError = "pixel format selection failed"; GlWindowDestroy(); return nullptr;
    }

    g_glrc = wglCreateContext(g_dc);
    if (!g_glrc || !wglMakeCurrent(g_dc, g_glrc)) {
        outError = "wglCreateContext/MakeCurrent failed"; GlWindowDestroy(); return nullptr;
    }

    if (!GlLoadFunctions()) { outError = "modern-GL functions missing"; GlWindowDestroy(); return nullptr; }

    // Disable vsync so the ~66 Hz driver timer paces us (no main-thread vsync stall).
    // Tearing is acceptable for the spike; vsync policy is a Phase 0.5 detail.
    typedef BOOL (WINAPI* PFNSWAPINTERVAL)(int);
    if (auto swapInterval = reinterpret_cast<PFNSWAPINTERVAL>(wglGetProcAddress("wglSwapIntervalEXT")))
        swapInterval(0);

    if (!g_renderer.Init(outError)) { GlWindowDestroy(); return nullptr; }

    spike::Model model;
    std::string loadErr;
    if (spike::LoadModel(fixturePath, model, loadErr)) {
        g_renderer.SetModel(model);
        ShowConsoleMsg(("[FBXAV-spike] (GL) loaded " + fixturePath + "\n").c_str());
    } else {
        ShowConsoleMsg(("[FBXAV-spike] (GL) load failed " + fixturePath + ": " + loadErr + "\n").c_str());
    }

    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    g_w = (rc.right > rc.left) ? (rc.right - rc.left) : 1;
    g_h = (rc.bottom > rc.top) ? (rc.bottom - rc.top) : 1;

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_lastFpsCount);
    g_frames = 0;
    g_fps = 0.0;

    return g_hwnd;
}

void GlWindowRenderFrame(float t)
{
    if (!g_hwnd || !g_dc || !g_glrc) return;  // g_hwnd nulled if the docker closed us
    wglMakeCurrent(g_dc, g_glrc);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);  // the window's default framebuffer
    g_renderer.DrawScene(t, g_w, g_h);
    SwapBuffers(g_dc);

    // Present-rate fps: count SwapBuffers, report once per second to the console.
    ++g_frames;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double elapsed = double(now.QuadPart - g_lastFpsCount.QuadPart) / double(g_freq.QuadPart);
    if (elapsed >= 1.0) {
        g_fps = g_frames / elapsed;
        g_frames = 0;
        g_lastFpsCount = now;
        char line[96];
        std::snprintf(line, sizeof(line), "[FBXAV-spike] (GL) %.1f fps  (%dx%d)\n", g_fps, g_w, g_h);
        ShowConsoleMsg(line);
    }
}

double GlWindowFps() { return g_fps; }

void GlWindowDestroy()
{
    if (g_glrc) {
        wglMakeCurrent(g_dc, g_glrc);
        g_renderer.Shutdown();
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(g_glrc);
        g_glrc = nullptr;
    }
    if (g_dc && g_hwnd) { ReleaseDC(g_hwnd, g_dc); g_dc = nullptr; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
    if (g_class_registered) { UnregisterClassW(kClass, g_hinst); g_class_registered = false; }
}

}  // namespace spike
