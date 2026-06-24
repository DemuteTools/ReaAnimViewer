// SPDX-License-Identifier: MIT
#pragma once

// Minimal Reaper API surface needed by Phase 0.
// Add REAPERAPI_WANT_<func> entries here as later phases require more.

#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_ShowConsoleMsg

// Story 1.3 — Reaper's native docker (resolved through the existing
// REAPERAPI_LoadAPI call; no new registration, just more wanted symbols).
#define REAPERAPI_WANT_DockWindowAddEx
#define REAPERAPI_WANT_DockWindowActivate
#define REAPERAPI_WANT_DockWindowRemove

// Refresh the toolbar/menu toggle state after the viewer toggle action runs.
#define REAPERAPI_WANT_RefreshToolbar

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
