// SPDX-License-Identifier: MIT
//
// Phase 0 viewer window: a modeless top-level Win32 window hosting an
// empty WGL context. No rendering beyond a fixed clear color — Phase 1
// adds mesh loading.

#include "viewer_window.h"

#ifdef _WIN32
#include <gl/GL.h>
#endif

namespace fbxav {
namespace {

constexpr wchar_t kWindowClassName[] = L"FBXAnimationViewer.ViewerWindow";
constexpr wchar_t kWindowTitle[]     = L"FBX Animation Viewer";

constexpr int kInitialWidth  = 800;
constexpr int kInitialHeight = 600;

HWND  g_hwnd  = nullptr;
HDC   g_hdc   = nullptr;
HGLRC g_hglrc = nullptr;
bool  g_class_registered = false;

void DestroyGLContext()
{
    if (g_hglrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(g_hglrc);
        g_hglrc = nullptr;
    }
    if (g_hdc && g_hwnd) {
        ReleaseDC(g_hwnd, g_hdc);
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

void PaintFrame()
{
    if (!g_hdc || !g_hglrc) return;
    wglMakeCurrent(g_hdc, g_hglrc);

    RECT rc;
    GetClientRect(g_hwnd, &rc);
    glViewport(0, 0, rc.right - rc.left, rc.bottom - rc.top);

    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    SwapBuffers(g_hdc);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        if (!CreateGLContextFor(hwnd)) {
            ShowConsoleMsg("[FBXAV] failed to create WGL context\n");
            return -1;  // abort window creation
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        PaintFrame();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;  // GL paints it; skip GDI background fill to avoid flicker.

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        DestroyGLContext();
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
    return true;
}

}  // namespace

void OpenViewerWindow(REAPER_PLUGIN_HINSTANCE hInst, HWND reaper_main)
{
    if (g_hwnd && IsWindow(g_hwnd)) {
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        return;
    }

    if (!EnsureClassRegistered(hInst)) {
        ShowConsoleMsg("[FBXAV] failed to register window class\n");
        return;
    }

    g_hwnd = CreateWindowExW(
        0, kWindowClassName, kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        kInitialWidth, kInitialHeight,
        reaper_main,            // parent — the Reaper main HWND
        nullptr, hInst, nullptr);

    if (!g_hwnd) {
        ShowConsoleMsg("[FBXAV] CreateWindowExW failed\n");
        return;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
}

void CloseViewerWindow()
{
    if (g_hwnd && IsWindow(g_hwnd)) {
        DestroyWindow(g_hwnd);
    }
    g_hwnd = nullptr;
}

}  // namespace fbxav
