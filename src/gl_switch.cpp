// SPDX-License-Identifier: MIT
//
// See gl_switch.h.

#include "gl_switch.h"

#include <chrono>
#include <mutex>

namespace rav {
namespace {

// Never destroyed: a video frame may still switch while the DLL's statics go at unload.
std::timed_mutex& g_switch = *new std::timed_mutex();

}  // namespace

BOOL GlMakeCurrent(HDC dc, HGLRC rc)
{
    if (!g_switch.try_lock_for(std::chrono::milliseconds(kGlSwitchWaitMs))) {
        SetLastError(WAIT_TIMEOUT);
        return FALSE;
    }
    const BOOL ok = wglMakeCurrent(dc, rc);
    const DWORD err = GetLastError();
    g_switch.unlock();
    SetLastError(err);
    return ok;
}

}  // namespace rav
