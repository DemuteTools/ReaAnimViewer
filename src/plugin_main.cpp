// SPDX-License-Identifier: MIT
//
// Reaper extension entry point. Phase 0: registers a single action that
// opens an empty WGL viewer window. No mesh loading, no transport sync.

#include "console_log.h"
#include "reaper_api.h"
#include "viewer_window.h"

namespace rav {
namespace {

constexpr const char kCommandName[] = "RAV_OPEN_VIEWER";
constexpr const char kActionDesc[]  = "RAV: Open Viewer";

int                     g_command_id      = 0;
gaccel_register_t       g_accel           = {};
REAPER_PLUGIN_HINSTANCE g_hinstance       = nullptr;
HWND                    g_reaper_main     = nullptr;

// Captured at load so the rec==nullptr unload path can deregister symmetrically.
int (*g_register)(const char*, void*) = nullptr;

bool OnHookCommand(int command, int /*flag*/)
{
    if (command != g_command_id || g_command_id == 0) return false;
    ToggleViewerWindow(g_hinstance, g_reaper_main);
    RefreshToolbar(g_command_id);  // reflect the new on/off state on the toolbar/menu now
    return true;
}

// Reaper polls this to draw the action's toggle checkmark / lit toolbar button.
int OnToggleAction(int command)
{
    if (command != g_command_id || g_command_id == 0) return -1;  // not ours / doesn't toggle
    return ViewerWindowIsVisible() ? 1 : 0;
}

}  // namespace
}  // namespace rav

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE hInstance,
    reaper_plugin_info_t*   rec)
{
    using namespace rav;

    if (!rec) {
        // Reaper is unloading us: tear down everything we registered, in
        // reverse order, so no live pointers into our DLL outlive it.
        CloseViewerWindow();
        if (g_register) {
            g_register("-toggleaction", (void*)&OnToggleAction);
            g_register("-hookcommand",  (void*)&OnHookCommand);
            g_register("-gaccel",       &g_accel);
        }
        return 0;
    }

    if (rec->caller_version != REAPER_PLUGIN_VERSION) return 0;

    if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) {
        // At least one required function failed to resolve. Bail before
        // we touch a null function pointer.
        return 0;
    }

    g_hinstance   = hInstance;
    g_reaper_main = rec->hwnd_main;
    g_register    = rec->Register;

    g_command_id = rec->Register("command_id", const_cast<char*>(kCommandName));
    if (g_command_id == 0) return 0;

    g_accel.accel.cmd = static_cast<WORD>(g_command_id);
    g_accel.desc      = kActionDesc;
    rec->Register("gaccel", &g_accel);
    rec->Register("hookcommand", (void*)&OnHookCommand);
    rec->Register("toggleaction", (void*)&OnToggleAction);

    LogInfo("extension loaded (Phase 0)");
    return 1;
}
