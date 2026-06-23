// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.
//
// Docked-OpenGL-window viewport experiment: renders the skinned rig DIRECTLY into
// a Reaper-docked window (no FBO, no readback) and SwapBuffers, to confirm 60 fps
// on-screen is reachable where the ReaImGui panel capped at ~30 fps.

#pragma once

#include <windows.h>
#include <string>

namespace spike {

// Creates a visible child window + WGL context, loads modern GL, and inits the
// renderer with the model at fixturePath. Returns the HWND (for the caller to dock
// via DockWindowAddEx) or nullptr on failure.
HWND GlWindowCreate(HINSTANCE hinst, HWND parent, const std::string& fixturePath,
                    std::string& outError);

// Renders one frame straight to the window and presents it. Tracks present fps.
void GlWindowRenderFrame(float t);

double GlWindowFps();

void GlWindowDestroy();

}  // namespace spike
