// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.

#include "spike_gl.h"

#include "reaper_api.h"  // ShowConsoleMsg

#define SPIKE_GL_DEF(ret, name, args) PFN_##name spike_##name = nullptr;
SPIKE_GL_FUNCS(SPIKE_GL_DEF)
#undef SPIKE_GL_DEF

namespace spike {
namespace {

constexpr wchar_t kGlWindowClass[] = L"FBXAV.Spike.GlHost";

HWND  g_hwnd  = nullptr;
HDC   g_hdc   = nullptr;
HGLRC g_hglrc = nullptr;
HINSTANCE g_hinst = nullptr;
bool  g_class_registered = false;

}  // namespace

bool GlLoadFunctions()
{
    bool ok = true;
#define SPIKE_GL_LOAD(ret, name, args)                                          \
    spike_##name = reinterpret_cast<PFN_##name>(wglGetProcAddress(#name));      \
    if (!spike_##name) { ShowConsoleMsg("[FBXAV-spike] missing GL func: " #name "\n"); ok = false; }
    SPIKE_GL_FUNCS(SPIKE_GL_LOAD)
#undef SPIKE_GL_LOAD
    return ok;
}

bool GlContextCreate(HINSTANCE hinst, HWND parent)
{
    g_hinst = hinst;

    if (!g_class_registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_OWNDC;
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = hinst;
        wc.lpszClassName = kGlWindowClass;
        if (!RegisterClassExW(&wc)) {
            const DWORD err = GetLastError();
            if (err != ERROR_CLASS_ALREADY_EXISTS) {
                ShowConsoleMsg("[FBXAV-spike] RegisterClassExW failed\n");
                return false;
            }
        }
        g_class_registered = true;
    }

    // Hidden host window — exists only to own a stable WGL DC/context. We never
    // present to it; sokol/GL renders to an offscreen FBO that we read back.
    g_hwnd = CreateWindowExW(0, kGlWindowClass, L"spike-gl", WS_OVERLAPPEDWINDOW,
                             0, 0, 16, 16, parent, nullptr, hinst, nullptr);
    if (!g_hwnd) { ShowConsoleMsg("[FBXAV-spike] CreateWindowExW failed\n"); return false; }

    g_hdc = GetDC(g_hwnd);
    if (!g_hdc) { ShowConsoleMsg("[FBXAV-spike] GetDC failed\n"); return false; }

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
        ShowConsoleMsg("[FBXAV-spike] pixel format selection failed\n");
        return false;
    }

    // Legacy context: the driver typically grants a high compatibility profile,
    // enough for #version 330 shaders. If shader compile fails on the reference
    // workstation, a wglCreateContextAttribsARB 3.3-core path is the fix — note
    // it in SPIKE0_FINDINGS.md (verification point).
    g_hglrc = wglCreateContext(g_hdc);
    if (!g_hglrc || !wglMakeCurrent(g_hdc, g_hglrc)) {
        ShowConsoleMsg("[FBXAV-spike] wglCreateContext/MakeCurrent failed\n");
        return false;
    }

    if (!GlLoadFunctions()) return false;

    ShowConsoleMsg("[FBXAV-spike] GL context up; modern-GL funcs resolved\n");
    return true;
}

bool GlMakeCurrent()
{
    if (!g_hdc || !g_hglrc) return false;
    return wglMakeCurrent(g_hdc, g_hglrc) != FALSE;
}

void GlContextDestroy()
{
    if (g_hglrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(g_hglrc);
        g_hglrc = nullptr;
    }
    if (g_hdc && g_hwnd) { ReleaseDC(g_hwnd, g_hdc); g_hdc = nullptr; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
    if (g_class_registered) {
        UnregisterClassW(kGlWindowClass, g_hinst);
        g_class_registered = false;
    }
}

}  // namespace spike
