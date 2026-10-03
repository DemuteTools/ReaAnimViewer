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

constexpr int kRegionRenderMatrixId = 41888;  // View: Show region render matrix window
constexpr int kRenderDialogId       = 40015;  // File: Render project to disk...
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
int ResolveAction(int known_id, bool (*matches)(const char*), const char* what)
{
    static bool s_logged[2] = {false, false};
    bool& logged = s_logged[known_id == kRegionRenderMatrixId ? 0 : 1];

    if (!g_get_text) {
        if (!logged) LogInfo("render windows: %s = action %d (name not checked: no kbd_getTextFromCmd)", what, known_id);
        logged = true;
        return known_id;
    }
    const char* name = g_get_text(known_id, nullptr);
    if (name && name[0]) {
        if (!logged)
            LogInfo("render windows: %s = action %d \"%s\"%s", what, known_id, name,
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
                    if (!logged) LogInfo("render windows: %s = action %d \"%s\" (found by name)", what, id, n);
                    logged = true;
                    return id;
                }
            }
        }
    }
    LogWarn("render windows: no REAPER action found for %s (action %d is \"%s\")", what, known_id, name ? name : "");
    return 0;
}

}  // namespace

void InitReaperActionLookup(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    g_get_text = reinterpret_cast<GetTextFromCmdFn>(get_func("kbd_getTextFromCmd"));
    g_enumerate = reinterpret_cast<EnumerateActionsFn>(get_func("kbd_enumerateActions"));
    g_section = reinterpret_cast<SectionFromUniqueIdFn>(get_func("SectionFromUniqueID"));
}

bool OpenRegionRenderMatrix()
{
    const int id = ResolveAction(kRegionRenderMatrixId, &IsRegionRenderMatrix, "Region Render Matrix");
    if (id <= 0) return false;
    // A toggle: when the window is already shown, turn it off and on so it comes to the
    // front (it may sit behind other windows or in a hidden docker tab).
    if (GetToggleCommandState(id) == 1) Main_OnCommand(id, 0);
    Main_OnCommand(id, 0);
    return true;
}

bool OpenRenderDialog()
{
    const int id = ResolveAction(kRenderDialogId, &IsRenderDialog, "Render dialog");
    if (id <= 0) return false;
    Main_OnCommand(id, 0);
    return true;
}

}  // namespace rav

#endif  // _WIN32
