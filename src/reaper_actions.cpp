// SPDX-License-Identifier: MIT
//
// See reaper_actions.h.

#include "reaper_actions.h"

#ifdef _WIN32

#include <cctype>
#include <cstring>
#include <string>

#include "console_log.h"
#include "reaper_api.h"

namespace rav {
namespace {

using GetTextFromCmdFn = const char* (*)(int cmd, KbdSectionInfo* section);
using EnumerateActionsFn = int (*)(KbdSectionInfo* section, int idx, const char** name_out);
using SectionFromUniqueIdFn = KbdSectionInfo* (*)(int unique_id);

GetTextFromCmdFn      g_get_text = nullptr;
EnumerateActionsFn    g_enumerate = nullptr;
SectionFromUniqueIdFn g_section = nullptr;
bool                  g_lookup_ready = false;  // InitReaperActionLookup ran

constexpr int kRegionRenderMatrixId = 41888;  // View: Show region render matrix window
constexpr int kRenderDialogId       = 40015;  // File: Render project to disk...
constexpr int kVideoWindowId        = 50125;  // Video: Show/hide video window (spec 11-fb-5)
constexpr int kMaxActionsScanned    = 200000;  // a guard: REAPER's Main section has a few thousand

std::string Lower(const char* s)
{
    std::string out(s ? s : "");
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool IsRegionRenderMatrix(const char* name)
{
    const std::string n = Lower(name);
    return n.find("render matrix") != std::string::npos;
}

bool IsVideoWindow(const char* name)
{
    // "Video: Show/hide video window" -- the toggle whose state says the window is open.
    return Lower(name) == "video: show/hide video window";
}

bool IsRenderDialog(const char* name)
{
    // "File: Render project to disk..." -- the dialog, not the "render using the most
    // recent settings" or "render queue" actions.
    const std::string n = Lower(name);
    if (n.rfind("file: render", 0) != 0) return false;
    return n.find("queue") == std::string::npos && n.find("most recent") == std::string::npos;
}

// The command id to run: the known one whenever REAPER knows it (any name: a LangPack
// translates action names, so a name that does not read as English is no reason to doubt
// the id), else the first Main-section action whose English name matches. 0 = not found.
// logged: the caller's own log-once flag.
int ResolveAction(int known_id, bool (*matches)(const char*), const char* what, bool& logged)
{
    if (!g_get_text) {
        if (!logged) LogInfo("REAPER action %s = %d (name not checked: no kbd_getTextFromCmd)", what, known_id);
        logged = true;
        return known_id;
    }
    const char* name = g_get_text(known_id, nullptr);
    if (name && name[0]) {
        if (!logged)
            LogInfo("REAPER action %s = %d \"%s\"%s", what, known_id, name,
                    matches(name) ? "" : " (name not the English one: translated REAPER?)");
        logged = true;
        return known_id;
    }
    if (g_enumerate && g_section) {
        KbdSectionInfo* main = g_section(0);
        if (main) {
            for (int i = 0; i < kMaxActionsScanned; ++i) {
                const char* n = nullptr;
                const int id = g_enumerate(main, i, &n);
                if (id <= 0) break;
                if (n && matches(n)) {
                    if (!logged) LogInfo("REAPER action %s = %d \"%s\" (found by name)", what, id, n);
                    logged = true;
                    return id;
                }
            }
        }
    }
    LogWarn("REAPER action %s: none found (action %d is \"%s\")", what, known_id, name ? name : "");
    return 0;
}

}  // namespace

void InitReaperActionLookup(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    g_get_text = reinterpret_cast<GetTextFromCmdFn>(get_func("kbd_getTextFromCmd"));
    g_enumerate = reinterpret_cast<EnumerateActionsFn>(get_func("kbd_enumerateActions"));
    g_section = reinterpret_cast<SectionFromUniqueIdFn>(get_func("SectionFromUniqueID"));
    g_lookup_ready = true;
}

bool OpenRegionRenderMatrix()
{
    static bool s_logged = false;
    const int id = ResolveAction(kRegionRenderMatrixId, &IsRegionRenderMatrix, "Region Render Matrix", s_logged);
    if (id <= 0) return false;
    // A toggle: when the window is already shown, turn it off and on so it comes to the
    // front (it may sit behind other windows or in a hidden docker tab).
    if (GetToggleCommandState(id) == 1) Main_OnCommand(id, 0);
    Main_OnCommand(id, 0);
    return true;
}

bool OpenRenderDialog()
{
    static bool s_logged = false;
    const int id = ResolveAction(kRenderDialogId, &IsRenderDialog, "Render dialog", s_logged);
    if (id <= 0) return false;
    Main_OnCommand(id, 0);
    return true;
}

bool ReaperVideoWindowOpen(bool* known)
{
    // Resolved once: called every frame. 0 = not found (warned once by ResolveAction).
    // Before InitReaperActionLookup nothing can be checked: state unknown, retried later.
    static int s_id = -1;
    static bool s_logged = false;
    if (s_id < 0) {
        if (!g_lookup_ready) {
            if (known) *known = false;
            return false;
        }
        s_id = ResolveAction(kVideoWindowId, &IsVideoWindow, "Video window", s_logged);
    }
    const int state = s_id > 0 ? GetToggleCommandState(s_id) : -1;
    static bool s_warned = false;
    if (s_id > 0 && state < 0 && !s_warned) {
        LogWarn("REAPER catch-up: action %d reports no toggle state; showing the readout while frames are requested",
                s_id);
        s_warned = true;
    }
    if (known) *known = state >= 0;
    return state == 1;
}

}  // namespace rav

#endif  // _WIN32
