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

#include "reaper_api.h"               // ShowConsoleMsg (REAPERAPI_MINIMAL)
#include "spike_gl.h"
#include "spike_loader.h"
#include "spike_renderer.h"

namespace fbxav {
namespace {

constexpr const char kCommandName[] = "FBXAV_SPIKE_OPEN";
constexpr const char kActionDesc[]  = "FBXAV: Open Spike Viewer (Spike 0)";

// Default fixture path (matches docs/SPIKE0_HOWTO_ANTHO.md). Override with the
// FBXAV_SPIKE_FIXTURE env var if you keep the file elsewhere.
constexpr const char kDefaultFixture[] = "D:/fixtures/skinned.glb";

int                     g_command_id = 0;
gaccel_register_t       g_accel      = {};
REAPER_PLUGIN_HINSTANCE g_hinstance  = nullptr;
HWND                    g_reaper_main = nullptr;
int (*g_register)(const char*, void*) = nullptr;
void* (*g_plugin_getapi)(const char*) = nullptr;

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

void RecreateImage(Panel& p, int w, int h)
{
    if (p.img) { ImGui::Detach(p.ctx, p.img); p.img = nullptr; }  // GC reclaims the old one
    p.img = ImGui::CreateImageFromSize(w, h);
    ImGui::Attach(p.ctx, p.img);
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
        char line[128];
        std::snprintf(line, sizeof(line), "fps: %.1f   (target >= 60)   %s",
                      p.fps, p.ready ? "" : "[no fixture loaded]");
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
            const uint32_t* px = p.renderer.RenderToPixels(static_cast<float>(t), rw, rh);
            if (px && rw == w && rh == h) {
                reaper_array* arr = p.EnsureArray(rw * rh);
                for (int i = 0; i < rw * rh; ++i) arr->data[i] = static_cast<double>(px[i]);
                ImGui::Image_SetPixels_Array(p.img, 0, 0, rw, rh, arr);
                ImGui::Image(p.ctx, p.img, rw, rh);
            }
        }
        ImGui::End(p.ctx);
    }

    if (!open) g_panel.reset();  // user closed the window
}

void Loop()
try {
    if (g_panel) Frame();
}
catch (const ImGui_Error& e) {
    ShowConsoleMsg("[FBXAV-spike] ImGui error: ");
    ShowConsoleMsg(e.what());
    ShowConsoleMsg("\n");
    g_panel.reset();
}

void StartPanel()
{
    if (g_panel) { ImGui::SetNextWindowFocus(g_panel->ctx); return; }

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
    g_register("timer", reinterpret_cast<void*>(&Loop));
    ShowConsoleMsg("[FBXAV-spike] panel started\n");
}

bool OnHookCommand(int command, int /*flag*/)
{
    if (command != g_command_id || g_command_id == 0) return false;
    StartPanel();
    return true;
}

}  // namespace
}  // namespace fbxav

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t* rec)
{
    using namespace fbxav;

    if (!rec) {
        if (g_panel) {
            if (g_register) g_register("-timer", reinterpret_cast<void*>(&Loop));
            g_panel->renderer.Shutdown();
            spike::GlContextDestroy();
            g_panel.reset();
        }
        if (g_register) {
            g_register("-hookcommand", reinterpret_cast<void*>(&OnHookCommand));
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
    rec->Register("hookcommand", reinterpret_cast<void*>(&OnHookCommand));

    ShowConsoleMsg("[FBXAV-spike] loaded (Spike 0). Run the action to open the viewer.\n");
    return 1;
}
