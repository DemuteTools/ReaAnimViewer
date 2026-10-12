// SPDX-License-Identifier: MIT
//
// See float_ui_window.h.

#include "float_ui_window.h"

#ifdef _WIN32

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>
#include <string>

#include <imgui_internal.h>  // ImGuiContext::IO (WantsText without making the context current)
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_win32.h>

#include "gl_loader.h"
#include "shortcuts.h"  // LoadPrefFloat / SavePrefFloat
#include "ui_theme.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace rav {

namespace {

constexpr wchar_t kClass[] = L"RAV_FloatUiWindow";

LRESULT CALLBACK FloatProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    if (FloatUiWindow* w = reinterpret_cast<FloatUiWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
        return w->Proc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::string Key(const char* pref, const char* what)
{
    return std::string(pref) + "." + what;
}

// Makes a context current for a scope, then puts the previous one back.
struct ContextScope {
    ImGuiContext* prev;
    explicit ContextScope(ImGuiContext* c) : prev(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(c); }
    ~ContextScope() { ImGui::SetCurrentContext(prev); }
};

}  // namespace

FloatUiWindow::FloatUiWindow(const char* pref, const wchar_t* title, int default_w, int default_h, int min_w, int min_h)
    : pref_(pref), title_(title), def_w_(default_w), def_h_(default_h), min_w_(min_w), min_h_(min_h)
{
}

void FloatUiWindow::Bind(HWND viewer, HDC viewer_dc, HGLRC rc)
{
    viewer_ = viewer;
    viewer_dc_ = viewer_dc;
    rc_ = rc;
}

bool FloatUiWindow::Create()
{
    if (!viewer_ || !viewer_dc_ || !rc_ || !ImGui::GetCurrentContext()) return false;
    HINSTANCE hi = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(viewer_, GWLP_HINSTANCE));
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC | CS_DBLCLKS;
        wc.lpfnWndProc = FloatProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        registered = true;
    }
    // The rect it had (screen pixels), else beside the viewer's right edge; back on a screen when its
    // monitor is gone.
    const float s = std::max(1.0f, ui::UiScale());
    RECT        vr = {};
    GetWindowRect(viewer_, &vr);
    int w = static_cast<int>(LoadPrefFloat(Key(pref_, "w").c_str(), static_cast<float>(def_w_) * s));
    int h = static_cast<int>(LoadPrefFloat(Key(pref_, "h").c_str(), static_cast<float>(def_h_) * s));
    w = std::max(w, static_cast<int>(min_w_ * s));
    h = std::max(h, static_cast<int>(min_h_ * s));
    int  x = static_cast<int>(LoadPrefFloat(Key(pref_, "x").c_str(), -100000.0f));
    int  y = static_cast<int>(LoadPrefFloat(Key(pref_, "y").c_str(), -100000.0f));
    RECT r = {x, y, x + w, y + h};
    if (x <= -99999 || y <= -99999 || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
        x = std::max(static_cast<int>(vr.left), static_cast<int>(vr.right) - w - static_cast<int>(24 * s));
        y = vr.top + static_cast<int>(40 * s);
    }
    HWND owner = GetAncestor(viewer_, GA_ROOT);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, title_, WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, x,
                            y, w, h, owner, nullptr, hi, this);
    if (!hwnd_) return false;
    dc_ = GetDC(hwnd_);
    // The viewer's pixel format: the shared GL context renders into either DC.
    const int             pf = GetPixelFormat(viewer_dc_);
    PIXELFORMATDESCRIPTOR pfd = {};
    if (!dc_ || !pf || !DescribePixelFormat(viewer_dc_, pf, sizeof(pfd), &pfd) || !SetPixelFormat(dc_, pf, &pfd)) {
        Destroy();
        return false;
    }
    // Its own ImGui context, on the viewer's font atlas and style.
    ImGuiIO&         mio = ImGui::GetIO();
    const ImGuiStyle style = ImGui::GetStyle();
    ImFont*          def_font = mio.FontDefault;
    atlas_ = mio.Fonts;
    main_tex_ = atlas_->TexID;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ctx_ = ImGui::CreateContext(atlas_);
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.FontDefault = def_font;
    ImGui::GetStyle() = style;
    bool ok = ImGui_ImplWin32_Init(hwnd_);
    if (ok) {
        ok = wglMakeCurrent(dc_, rc_) && ImGui_ImplOpenGL3_Init("#version 130");
        wglMakeCurrent(viewer_dc_, rc_);
        if (!ok) ImGui_ImplWin32_Shutdown();
    }
    if (!ok) {
        ImGui::DestroyContext(ctx_);
        ctx_ = nullptr;
        ImGui::SetCurrentContext(prev);
        Destroy();
        return false;
    }
    ImGui::SetCurrentContext(prev);
    return true;
}

void FloatUiWindow::Frame(bool show, bool activate, const std::function<void()>& draw)
{
    if (!show) {
        Hide();
        return;
    }
    if (!hwnd_) {
        if (failed_) return;
        if (!Create()) {
            failed_ = true;
            return;
        }
    }
    if (!IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    } else if (activate) {
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
    }
    if (activate) SetFocus(hwnd_);

    ContextScope scope(ctx_);
    if (!wglMakeCurrent(dc_, rc_)) {
        wglMakeCurrent(viewer_dc_, rc_);
        return;
    }
    ImGui_ImplOpenGL3_NewFrame();
    atlas_->SetTexID(main_tex_);  // its backend made a texture of its own: both read the viewer's, alive in the shared context
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (ui::BeginToolWindowFill("##floatui")) draw();
    ui::EndToolWindowFill();
    ImGui::Render();
    const ImGuiIO& io = ImGui::GetIO();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, static_cast<int>(io.DisplaySize.x * io.DisplayFramebufferScale.x),
               static_cast<int>(io.DisplaySize.y * io.DisplayFramebufferScale.y));
    const ImVec4 bg = ui::Col(ui::kRaised);
    glClearColor(bg.x, bg.y, bg.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SwapBuffers(dc_);
    wglMakeCurrent(viewer_dc_, rc_);
    if (rect_dirty_ && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) SaveRect();
}

void FloatUiWindow::SetDragArea(float x0, float y0, float x1, float y1, bool over_widget)
{
    drag_[0] = x0;
    drag_[1] = y0;
    drag_[2] = x1;
    drag_[3] = y1;
    drag_over_widget_ = over_widget;
}

bool FloatUiWindow::TakeCloseRequest()
{
    const bool r = close_requested_;
    close_requested_ = false;
    return r;
}

bool FloatUiWindow::HasFocus() const
{
    if (!hwnd_ || !IsWindowVisible(hwnd_)) return false;
    const HWND f = GetFocus();
    return f == hwnd_ || (f && IsChild(hwnd_, f));
}

bool FloatUiWindow::WantsText() const
{
    return ctx_ && ctx_->IO.WantTextInput;
}

bool FloatUiWindow::IsShown() const
{
    return hwnd_ && IsWindowVisible(hwnd_);
}

void FloatUiWindow::Hide()
{
    if (!hwnd_ || !IsWindowVisible(hwnd_)) return;
    if (rect_dirty_) SaveRect();
    ShowWindow(hwnd_, SW_HIDE);
}

void FloatUiWindow::Destroy()
{
    if (ctx_) {
        ContextScope scope(ctx_);
        if (dc_ && rc_ && wglMakeCurrent(dc_, rc_)) {
            ImGui_ImplOpenGL3_Shutdown();
            wglMakeCurrent(viewer_dc_, rc_);
        }
        ImGui_ImplWin32_Shutdown();
        if (atlas_) atlas_->SetTexID(main_tex_);  // its backend cleared the shared atlas's texture: the viewer's again
    }
    if (ctx_) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::DestroyContext(ctx_);  // the shared atlas is not its own: kept
        ImGui::SetCurrentContext(prev == ctx_ ? nullptr : prev);
        ctx_ = nullptr;
    }
    if (hwnd_) {
        if (rect_dirty_) SaveRect();
        if (dc_) ReleaseDC(hwnd_, dc_);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    dc_ = nullptr;
    atlas_ = nullptr;
    failed_ = false;
}

void FloatUiWindow::SaveRect()
{
    rect_dirty_ = false;
    RECT r = {};
    if (!hwnd_ || !GetWindowRect(hwnd_, &r)) return;
    SavePrefFloat(Key(pref_, "x").c_str(), static_cast<float>(r.left));
    SavePrefFloat(Key(pref_, "y").c_str(), static_cast<float>(r.top));
    SavePrefFloat(Key(pref_, "w").c_str(), static_cast<float>(r.right - r.left));
    SavePrefFloat(Key(pref_, "h").c_str(), static_cast<float>(r.bottom - r.top));
}

LRESULT FloatUiWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ctx_) {
        ContextScope scope(ctx_);
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    }
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;  // no system frame: the client area is the whole window (the edges still resize)
        break;
    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        const int  b = std::max(4, static_cast<int>(6.0f * std::max(1.0f, ui::UiScale())));
        const bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, d = pt.y >= rc.bottom - b;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (d && l) return HTBOTTOMLEFT;
        if (d && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t) return HTTOP;
        if (d) return HTBOTTOM;
        if (!drag_over_widget_ && pt.x >= drag_[0] && pt.x < drag_[2] && pt.y >= drag_[1] && pt.y < drag_[3]) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
        const float s = std::max(1.0f, ui::UiScale());
        mm->ptMinTrackSize.x = static_cast<LONG>(min_w_ * s);
        mm->ptMinTrackSize.y = static_cast<LONG>(min_h_ * s);
        return 0;
    }
    case WM_MOVE:
    case WM_SIZE:
        rect_dirty_ = true;
        break;
    case WM_EXITSIZEMOVE:
        SaveRect();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:  // Alt+F4: hidden, and the content is told (TakeCloseRequest)
        close_requested_ = true;
        Hide();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace rav

#endif  // _WIN32
