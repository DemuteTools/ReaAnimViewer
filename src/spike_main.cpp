// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.
//
// Reaper entry point for the feasibility spike. Opens a dockable ReaImGui panel
// that shows a GPU-skinned glTF animating, bridged in via CPU readback (the only
// mechanism ReaImGui supports — see docs/SPIKE0_FINDINGS.md), with an fps readout.
//
// BUILD PREREQUISITE: drop the generated ReaImGui binding header at
//   extern/reaimgui/include/reaper_imgui_functions.h
// (from https://github.com/cfillion/reaimgui/releases, or REAPER action
//  "[developer] Write C++ API functions header"). It is generated, not in the
// ReaImGui source tree, so it is not vendored here.

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

#define REAIMGUIAPI_IMPLEMENT
#include "reaper_imgui_functions.h"   // provides ImGui:: namespace + reaper_array

#include "reaper_api.h"               // ShowConsoleMsg + Dock* (REAPERAPI_MINIMAL)
#include "spike_gl.h"
#include "spike_glwindow.h"
#include "spike_loader.h"
#include "spike_renderer.h"

// reaper_imgui_functions.h only forward-declares reaper_array; provide its
// canonical REAPER layout so we can fill the pixel array (binary-compatible).
struct reaper_array { unsigned int size, alloc; double data[1]; };

namespace fbxav {
namespace {

constexpr const char kCommandName[] = "FBXAV_SPIKE_OPEN";
constexpr const char kActionDesc[]  = "FBXAV: Open Spike Viewer (ReaImGui, ~30fps)";

constexpr const char kGlCommandName[] = "FBXAV_SPIKE_OPEN_GL";
constexpr const char kGlActionDesc[]  = "FBXAV: Open Spike Viewer (GL docked, 60fps test)";

enum class Mode { None, ImGuiPanel, GlWindow };
Mode g_mode = Mode::None;
HWND g_gl_hwnd = nullptr;

int               g_gl_command_id = 0;
gaccel_register_t g_gl_accel      = {};

// Default fixture path (matches docs/SPIKE0_HOWTO_ANTHO.md). Override with the
// FBXAV_SPIKE_FIXTURE env var if you keep the file elsewhere. assimp sniffs the
// format from content, so .fbx (Mixamo) and .glb/.gltf all work.
constexpr const char kDefaultFixture[] = "D:/fixtures/skinned.fbx";

int                     g_command_id = 0;
gaccel_register_t       g_accel      = {};
REAPER_PLUGIN_HINSTANCE g_hinstance  = nullptr;
HWND                    g_reaper_main = nullptr;
int (*g_register)(const char*, void*) = nullptr;
void* (*g_plugin_getapi)(const char*) = nullptr;
UINT_PTR g_timer_id = 0;   // Spike experiment: our own fast timer (see StartPanel)

LARGE_INTEGER g_qpcFreq{}, g_qpcStart{};

double NowSeconds()
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return double(c.QuadPart - g_qpcStart.QuadPart) / double(g_qpcFreq.QuadPart);
}

// One-at-a-time spike panel.
struct Panel {
    ImGui_Context* ctx = nullptr;
    ImGui_Image*   img = nullptr;
    spike::Renderer renderer;
    int   imgW = 0, imgH = 0;
    bool  ready = false;
    double fps = 0.0, lastT = 0.0;
    double renderMs = 0.0;   // measured cost of render+readback+upload (vs tick rate)
    std::vector<unsigned char> arrayBuf;  // reaper_array storage (size + doubles)

    reaper_array* EnsureArray(int n)
    {
        const size_t bytes = offsetof(reaper_array, data) + size_t(n) * sizeof(double);
        if (arrayBuf.size() < bytes) arrayBuf.resize(bytes);
        auto* arr = reinterpret_cast<reaper_array*>(arrayBuf.data());
        arr->size = arr->alloc = static_cast<unsigned>(n);
        return arr;
    }
};

std::unique_ptr<Panel> g_panel;

void StopAll()
{
    if (g_timer_id) { KillTimer(nullptr, g_timer_id); g_timer_id = 0; }
    if (g_panel) {                       // ReaImGui path
        g_panel->renderer.Shutdown();
        spike::GlContextDestroy();
        g_panel.reset();
    }
    if (g_gl_hwnd) {                      // docked-GL-window path
        DockWindowRemove(g_gl_hwnd);
        spike::GlWindowDestroy();
        g_gl_hwnd = nullptr;
    }
    g_mode = Mode::None;
}

void RecreateImage(Panel& p, int w, int h)
{
    // ReaImGui images stay valid while used each defer cycle, so no Attach is
    // needed; the previous image is GC'd once we stop referencing it on resize.
    p.img = ImGui::CreateImageFromSize(w, h);
    p.imgW = w; p.imgH = h;
}

void Frame()  // called every Reaper timer tick
{
    Panel& p = *g_panel;

    const double t = NowSeconds();
    const double dt = t - p.lastT;
    p.lastT = t;
    if (dt > 0.0) p.fps = p.fps * 0.9 + (1.0 / dt) * 0.1;  // smoothed

    ImGui::SetNextWindowSize(p.ctx, 720, 540, ImGui::Cond_FirstUseEver);

    bool open = true;
    if (ImGui::Begin(p.ctx, "FBXAV Spike Viewer", &open)) {
        char line[224];
        const double cap = (p.renderMs > 0.01) ? 1000.0 / p.renderMs : 0.0;
        const double imguiFps = ImGui::GetFramerate(p.ctx);  // ReaImGui's own present-rate estimate
        std::snprintf(line, sizeof(line),
            "tick: %.0f fps  |  imgui: %.0f fps  |  render+readback: %.1f ms (cap ~%.0f fps)  |  %dx%d  %s",
            p.fps, imguiFps, p.renderMs, cap, p.imgW, p.imgH, p.ready ? "" : "[no fixture]");
        ImGui::Text(p.ctx, line);

        double availX = 0, availY = 0;
        ImGui::GetContentRegionAvail(p.ctx, &availX, &availY);
        const int w = static_cast<int>(availX), h = static_cast<int>(availY);

        if (p.ready && w > 1 && h > 1) {
            if (w != p.imgW || h != p.imgH) {
                p.renderer.Resize(w, h);
                RecreateImage(p, w, h);
            }
            int rw = 0, rh = 0;
            const double r0 = NowSeconds();
            const uint32_t* px = p.renderer.RenderToPixels(static_cast<float>(t), rw, rh);
            if (px && rw == w && rh == h) {
                reaper_array* arr = p.EnsureArray(rw * rh);
                for (int i = 0; i < rw * rh; ++i) arr->data[i] = static_cast<double>(px[i]);
                // Image_SetPixels_Array wants an ImGui_Bitmap*; CreateImageFromSize
                // returns the same object typed as ImGui_Image* (display API).
                ImGui::Image_SetPixels_Array(reinterpret_cast<ImGui_Bitmap*>(p.img),
                                             0, 0, rw, rh, arr);
                const double r1 = NowSeconds();
                p.renderMs = p.renderMs * 0.9 + (r1 - r0) * 1000.0 * 0.1;  // EMA of true render cost
                ImGui::Image(p.ctx, p.img, rw, rh);
            }
        }
        ImGui::End(p.ctx);
    }

    if (!open) StopAll();  // user closed the window
}

void Loop()
try {
    if (g_panel) Frame();
}
catch (const ImGui_Error& e) {
    ShowConsoleMsg("[FBXAV-spike] ImGui error: ");
    ShowConsoleMsg(e.what());
    ShowConsoleMsg("\n");
    StopAll();
}

void CALLBACK SpikeTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    if (g_mode == Mode::ImGuiPanel)      Loop();
    else if (g_mode == Mode::GlWindow)   spike::GlWindowRenderFrame(static_cast<float>(NowSeconds()));
}

void StartPanel()
{
    if (g_mode == Mode::ImGuiPanel && g_panel) { ImGui::SetNextWindowFocus(g_panel->ctx); return; }
    StopAll();  // close the GL window first if it was open (one viewer at a time)

    auto p = std::make_unique<Panel>();

    std::string err;
    if (!spike::GlContextCreate(g_hinstance, g_reaper_main)) {
        ShowConsoleMsg("[FBXAV-spike] GL context creation failed\n");
        return;
    }
    if (!p->renderer.Init(err)) {
        ShowConsoleMsg(("[FBXAV-spike] renderer init failed: " + err + "\n").c_str());
        spike::GlContextDestroy();
        return;
    }

    const char* envPath = std::getenv("FBXAV_SPIKE_FIXTURE");
    const std::string path = envPath ? envPath : kDefaultFixture;
    spike::Model model;
    if (spike::LoadModel(path, model, err)) {
        p->renderer.SetModel(model);
        p->ready = true;
        ShowConsoleMsg(("[FBXAV-spike] loaded " + path + "\n").c_str());
    } else {
        ShowConsoleMsg(("[FBXAV-spike] load failed " + path + ": " + err + "\n").c_str());
    }

    try {
        ImGui::init(g_plugin_getapi);
        p->ctx = ImGui::CreateContext("FBXAV Spike", ImGui::ConfigFlags_DockingEnable);
    }
    catch (const ImGui_Error& e) {
        ShowConsoleMsg("[FBXAV-spike] ReaImGui unavailable — install cfillion/reaimgui: ");
        ShowConsoleMsg(e.what()); ShowConsoleMsg("\n");
        spike::GlContextDestroy();
        return;
    }

    p->lastT = NowSeconds();
    g_panel = std::move(p);

    // Experiment (Spike 0): drive the frame from our own ~66 Hz Win32 timer instead
    // of Reaper's ~30 Hz extension timer, to test whether ReaImGui content can exceed
    // 30 fps. Reaper's main message loop dispatches the WM_TIMER to our callback.
    g_mode = Mode::ImGuiPanel;
    g_timer_id = SetTimer(nullptr, 0, 15, &SpikeTimerProc);
    if (!g_timer_id) ShowConsoleMsg("[FBXAV-spike] SetTimer failed\n");
    ShowConsoleMsg("[FBXAV-spike] ReaImGui panel started (66 Hz timer)\n");
}

void StartGlWindow()
{
    if (g_mode == Mode::GlWindow && g_gl_hwnd) { DockWindowActivate(g_gl_hwnd); return; }
    StopAll();  // close the ReaImGui panel first if it was open

    const char* envPath = std::getenv("FBXAV_SPIKE_FIXTURE");
    const std::string path = envPath ? envPath : kDefaultFixture;

    std::string err;
    HWND hwnd = spike::GlWindowCreate(g_hinstance, g_reaper_main, path, err);
    if (!hwnd) {
        ShowConsoleMsg(("[FBXAV-spike] GL window failed: " + err + "\n").c_str());
        return;
    }
    g_gl_hwnd = hwnd;

    // Hand the raw GL window to Reaper's docker (this is the docked-window route
    // that lets us drive our own present loop, unlike a ReaImGui panel).
    DockWindowAddEx(hwnd, "FBXAV Spike (GL)", "fbxav_spike_gl", true);
    DockWindowActivate(hwnd);

    g_mode = Mode::GlWindow;
    g_timer_id = SetTimer(nullptr, 0, 15, &SpikeTimerProc);
    if (!g_timer_id) ShowConsoleMsg("[FBXAV-spike] SetTimer failed\n");
    ShowConsoleMsg("[FBXAV-spike] GL docked window started (fps logged here each second)\n");
}

bool OnHookCommand(int command, int /*flag*/)
{
    if (command == 0) return false;
    if (command == g_command_id)    { StartPanel();    return true; }
    if (command == g_gl_command_id) { StartGlWindow(); return true; }
    return false;
}

}  // namespace
}  // namespace fbxav

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t* rec)
{
    using namespace fbxav;

    if (!rec) {
        StopAll();
        if (g_register) {
            g_register("-hookcommand", reinterpret_cast<void*>(&OnHookCommand));
            g_register("-gaccel", &g_gl_accel);
            g_register("-gaccel", &g_accel);
        }
        return 0;
    }

    if (rec->caller_version != REAPER_PLUGIN_VERSION) return 0;
    if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) return 0;

    g_hinstance   = hInstance;
    g_reaper_main = rec->hwnd_main;
    g_register    = rec->Register;
    g_plugin_getapi = reinterpret_cast<decltype(g_plugin_getapi)>(rec->GetFunc("plugin_getapi"));
    if (!g_plugin_getapi) { ShowConsoleMsg("[FBXAV-spike] plugin_getapi missing\n"); return 0; }

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);

    g_command_id = rec->Register("command_id", const_cast<char*>(kCommandName));
    if (g_command_id == 0) return 0;
    g_accel.accel.cmd = static_cast<WORD>(g_command_id);
    g_accel.desc      = kActionDesc;
    rec->Register("gaccel", &g_accel);

    g_gl_command_id = rec->Register("command_id", const_cast<char*>(kGlCommandName));
    if (g_gl_command_id != 0) {
        g_gl_accel.accel.cmd = static_cast<WORD>(g_gl_command_id);
        g_gl_accel.desc      = kGlActionDesc;
        rec->Register("gaccel", &g_gl_accel);
    }

    rec->Register("hookcommand", reinterpret_cast<void*>(&OnHookCommand));

    ShowConsoleMsg("[FBXAV-spike] loaded (Spike 0). Two actions: ReaImGui (~30fps) and GL docked (60fps test).\n");
    return 1;
}
