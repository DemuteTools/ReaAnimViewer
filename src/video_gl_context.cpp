// SPDX-License-Identifier: MIT
//
// See video_gl_context.h. Pattern proven by spike 11-0 (video_gl.cpp), production
// version: own window class, per-thread state, depth attachment, rowspan-aware
// readback. Every failure turns into "no GL" for the caller. Context failures are
// logged once per thread and GL errors at most five times; a target size the driver
// refuses warns each time it is retried (the log ring merges the repeats).

#include "video_gl_context.h"

#include <windows.h>
#include <GL/gl.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "console_log.h"

#ifndef APIENTRY
#define APIENTRY __stdcall
#endif

namespace rav {
namespace {

// GL 3.0 / ARB_framebuffer_object values (not in the Windows GL 1.1 header).
constexpr GLenum kFramebuffer         = 0x8D40;
constexpr GLenum kRenderbuffer        = 0x8D41;
constexpr GLenum kColorAttachment0    = 0x8CE0;
constexpr GLenum kDepthAttachment     = 0x8D00;
constexpr GLenum kFramebufferComplete = 0x8CD5;
constexpr GLenum kRgba8               = 0x8058;
constexpr GLenum kDepthComponent24    = 0x81A6;
constexpr GLenum kBgra                = 0x80E1;

typedef void   (APIENTRY* FnGenFramebuffers)(GLsizei, GLuint*);
typedef void   (APIENTRY* FnDeleteFramebuffers)(GLsizei, const GLuint*);
typedef void   (APIENTRY* FnBindFramebuffer)(GLenum, GLuint);
typedef GLenum (APIENTRY* FnCheckFramebufferStatus)(GLenum);
typedef void   (APIENTRY* FnFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef void   (APIENTRY* FnGenRenderbuffers)(GLsizei, GLuint*);
typedef void   (APIENTRY* FnDeleteRenderbuffers)(GLsizei, const GLuint*);
typedef void   (APIENTRY* FnBindRenderbuffer)(GLenum, GLuint);
typedef void   (APIENTRY* FnRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);

const wchar_t kWindowClass[] = L"RAV_VideoFx_GL";

// One per thread that renders video frames. Deliberately never torn down from a
// thread_local destructor: those run under the loader lock at thread exit, where
// wglDeleteContext / DestroyWindow can deadlock with the driver. REAPER's video
// thread lives as long as the session; the context goes with the process.
struct ThreadGl {
    bool  attempted = false;
    bool  ok        = false;
    bool  logged_failure = false;
    int   gl_errors_logged = 0;
    HWND  hwnd = nullptr;
    HDC   hdc  = nullptr;
    HGLRC rc   = nullptr;
    GLuint fbo = 0;
    GLuint color_rb = 0;
    GLuint depth_rb = 0;
    int    fb_w = 0;
    int    fb_h = 0;
    FnGenFramebuffers         GenFramebuffers         = nullptr;
    FnDeleteFramebuffers      DeleteFramebuffers      = nullptr;
    FnBindFramebuffer         BindFramebuffer         = nullptr;
    FnCheckFramebufferStatus  CheckFramebufferStatus  = nullptr;
    FnFramebufferRenderbuffer FramebufferRenderbuffer = nullptr;
    FnGenRenderbuffers        GenRenderbuffers        = nullptr;
    FnDeleteRenderbuffers     DeleteRenderbuffers     = nullptr;
    FnBindRenderbuffer        BindRenderbuffer        = nullptr;
    FnRenderbufferStorage     RenderbufferStorage     = nullptr;
    std::vector<unsigned char> staging;
};

thread_local ThreadGl t_gl;

HINSTANCE ModuleInstance()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleInstance), &self);
    return self;
}

template <typename T>
bool LoadProc(T& out, const char* name)
{
    PROC p = wglGetProcAddress(name);
    // Some drivers return small sentinel values instead of NULL on failure.
    const intptr_t v = reinterpret_cast<intptr_t>(p);
    if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) return false;
    out = reinterpret_cast<T>(p);
    return true;
}

void RestoreContext(HDC dc, HGLRC rc)
{
    if (rc) wglMakeCurrent(dc, rc);
    else wglMakeCurrent(nullptr, nullptr);
}

void LogFailureOnce(ThreadGl& g, const char* what, DWORD err)
{
    if (g.logged_failure) return;
    g.logged_failure = true;
    LogWarn("video FX: no OpenGL on the video thread (%s, error %lu): frames are drawn without GL",
            what, static_cast<unsigned long>(err));
}

// Hidden window + WGL context for the calling thread, FBO entry points loaded.
bool CreateThreadContext(ThreadGl& g)
{
    HINSTANCE hinst = ModuleInstance();
    WNDCLASSW wc = {};
    wc.style = CS_OWNDC;
    // DefWindowProcW (user32), not a function of this DLL: the window never points
    // into code that could be unloaded under it.
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hinst;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogFailureOnce(g, "RegisterClass", GetLastError());
        return false;
    }
    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"RAV video FX", WS_POPUP, 0, 0, 1, 1,
                             nullptr, nullptr, hinst, nullptr);
    if (!g.hwnd) {
        LogFailureOnce(g, "CreateWindowEx", GetLastError());
        return false;
    }
    g.hdc = GetDC(g.hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int pf = g.hdc ? ChoosePixelFormat(g.hdc, &pfd) : 0;
    if (!pf || !SetPixelFormat(g.hdc, pf, &pfd)) {
        LogFailureOnce(g, "pixel format", GetLastError());
        return false;
    }
    g.rc = wglCreateContext(g.hdc);
    if (!g.rc) {
        LogFailureOnce(g, "wglCreateContext", GetLastError());
        return false;
    }

    HGLRC prev_rc = wglGetCurrentContext();
    HDC   prev_dc = wglGetCurrentDC();
    if (!wglMakeCurrent(g.hdc, g.rc)) {
        LogFailureOnce(g, "wglMakeCurrent", GetLastError());
        RestoreContext(prev_dc, prev_rc);  // a failed MakeCurrent un-currents the old one
        return false;
    }
    const bool loaded = LoadProc(g.GenFramebuffers, "glGenFramebuffers") &&
                        LoadProc(g.DeleteFramebuffers, "glDeleteFramebuffers") &&
                        LoadProc(g.BindFramebuffer, "glBindFramebuffer") &&
                        LoadProc(g.CheckFramebufferStatus, "glCheckFramebufferStatus") &&
                        LoadProc(g.FramebufferRenderbuffer, "glFramebufferRenderbuffer") &&
                        LoadProc(g.GenRenderbuffers, "glGenRenderbuffers") &&
                        LoadProc(g.DeleteRenderbuffers, "glDeleteRenderbuffers") &&
                        LoadProc(g.BindRenderbuffer, "glBindRenderbuffer") &&
                        LoadProc(g.RenderbufferStorage, "glRenderbufferStorage");
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    RestoreContext(prev_dc, prev_rc);
    if (!loaded) {
        LogFailureOnce(g, "framebuffer functions missing", 0);
        return false;
    }
    LogInfo("video FX: GL context created on thread %lu (%s)", static_cast<unsigned long>(GetCurrentThreadId()),
            version ? version : "?");
    return true;
}

// Undoes a CreateThreadContext that failed part way. A hidden top-level window left
// behind would never be pumped again (Begin stops at !g.ok), and a broadcast
// SendMessage from another program would then wait on it forever.
void DestroyThreadContext(ThreadGl& g)
{
    if (g.rc) wglDeleteContext(g.rc);  // never current here: failures restore the old one
    if (g.hdc) ReleaseDC(g.hwnd, g.hdc);
    if (g.hwnd) DestroyWindow(g.hwnd);
    g.rc   = nullptr;
    g.hdc  = nullptr;
    g.hwnd = nullptr;
}

// Called with this thread's context current.
bool EnsureTarget(ThreadGl& g, int w, int h)
{
    if (g.fbo && g.fb_w == w && g.fb_h == h) return true;
    if (!g.fbo) {
        g.GenFramebuffers(1, &g.fbo);
        g.GenRenderbuffers(1, &g.color_rb);
        g.GenRenderbuffers(1, &g.depth_rb);
    }
    g.BindRenderbuffer(kRenderbuffer, g.color_rb);
    g.RenderbufferStorage(kRenderbuffer, kRgba8, w, h);
    g.BindRenderbuffer(kRenderbuffer, g.depth_rb);
    g.RenderbufferStorage(kRenderbuffer, kDepthComponent24, w, h);
    g.BindRenderbuffer(kRenderbuffer, 0);
    g.BindFramebuffer(kFramebuffer, g.fbo);
    g.FramebufferRenderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, g.color_rb);
    g.FramebufferRenderbuffer(kFramebuffer, kDepthAttachment, kRenderbuffer, g.depth_rb);
    const GLenum status = g.CheckFramebufferStatus(kFramebuffer);
    g.BindFramebuffer(kFramebuffer, 0);
    if (status != kFramebufferComplete) {
        LogWarn("video FX: %dx%d target incomplete (status 0x%x)", w, h, static_cast<unsigned>(status));
        g.fb_w = g.fb_h = 0;
        return false;
    }
    g.fb_w = w;
    g.fb_h = h;
    return true;
}

}  // namespace

VideoGlFrame::~VideoGlFrame()
{
    if (!active_) return;
    ThreadGl& g = t_gl;
    if (g.BindFramebuffer) g.BindFramebuffer(kFramebuffer, 0);
    RestoreContext(static_cast<HDC>(prev_dc_), static_cast<HGLRC>(prev_rc_));
}

bool VideoGlFrame::Begin(int width, int height)
{
    if (active_ || width <= 0 || height <= 0) return false;
#ifdef RAV_FORCE_INIT_FAILURE
    // TEST-ONLY (build.bat forcefail): no GL on the video thread either, so the CPU
    // fallback of the test pattern can be checked on working hardware.
    return false;
#endif
    ThreadGl& g = t_gl;
    if (!g.attempted) {
        g.attempted = true;
        g.ok = CreateThreadContext(g);
        if (!g.ok) DestroyThreadContext(g);
    }
    if (!g.ok) return false;

    // The video thread pumps no messages of its own: dispatch anything sent to the
    // hidden window (a broadcast SendMessage would otherwise wait on it).
    MSG msg;
    while (PeekMessageW(&msg, g.hwnd, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

    HGLRC prev_rc = wglGetCurrentContext();
    HDC   prev_dc = wglGetCurrentDC();
    if (!wglMakeCurrent(g.hdc, g.rc)) {
        LogFailureOnce(g, "wglMakeCurrent per frame", GetLastError());
        RestoreContext(prev_dc, prev_rc);
        return false;
    }
    prev_dc_ = prev_dc;
    prev_rc_ = prev_rc;
    active_  = true;  // from here on the destructor restores the previous context
    width_   = width;
    height_  = height;

    // Start the frame with a clean error queue, so ReadInto reports this frame only.
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}

    if (!EnsureTarget(g, width, height)) return false;
    g.BindFramebuffer(kFramebuffer, g.fbo);
    glViewport(0, 0, width, height);
    return true;
}

unsigned VideoGlFrame::Fbo() const
{
    return active_ ? t_gl.fbo : 0;
}

bool VideoGlFrame::ReadInto(unsigned char* pixels, int row_bytes)
{
    if (!active_ || !pixels || row_bytes < width_ * 4) return false;
    ThreadGl& g = t_gl;
    const size_t src_row = static_cast<size_t>(width_) * 4;
    try {
        g.staging.resize(src_row * static_cast<size_t>(height_));
    } catch (...) {
        return false;
    }
    glFinish();
    glReadBuffer(kColorAttachment0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    // GL_BGRA = bytes B,G,R,A = LICE's 0xAARRGGBB, the layout REAPER's 'RGBA' frames use.
    glReadPixels(0, 0, width_, height_, kBgra, GL_UNSIGNED_BYTE, g.staging.data());
    GLenum err = GL_NO_ERROR;
    for (int i = 0; i < 16; ++i) {  // drain the queue; keep the first error
        const GLenum e = glGetError();
        if (e == GL_NO_ERROR) break;
        if (err == GL_NO_ERROR) err = e;
    }
    if (err != GL_NO_ERROR) {
        if (g.gl_errors_logged < 5) {
            ++g.gl_errors_logged;
            LogWarn("video FX: GL error 0x%x on a video frame", static_cast<unsigned>(err));
        }
        return false;
    }
    // GL rows are bottom-up; REAPER frames are top-down.
    for (int y = 0; y < height_; ++y) {
        const unsigned char* src = g.staging.data() + static_cast<size_t>(height_ - 1 - y) * src_row;
        std::memcpy(pixels + static_cast<size_t>(y) * static_cast<size_t>(row_bytes), src, src_row);
    }
    return true;
}

}  // namespace rav
