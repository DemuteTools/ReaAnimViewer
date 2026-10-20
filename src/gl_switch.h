// SPDX-License-Identifier: MIT
//
// wglMakeCurrent, never on two of RAV's threads at once (freeze report 2026-10-21: REAPER froze for good
// with its UI thread spinning inside the AMD driver's wglMakeCurrent -- a floating window switching back
// to the viewer's DC -- while REAPER's video thread sat in the same call for the video FX's frame; the
// sound played on). Every wglMakeCurrent in RAV goes through GlMakeCurrent: one process-wide lock around
// the call only. A thread that cannot take it within kGlSwitchWaitMs gets FALSE (GetLastError
// WAIT_TIMEOUT), as for a refused switch: the caller skips that frame, it never waits for ever.

#pragma once

#include <windows.h>

namespace rav {

constexpr unsigned kGlSwitchWaitMs = 500;

BOOL GlMakeCurrent(HDC dc, HGLRC rc);

}  // namespace rav
