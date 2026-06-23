// SPDX-License-Identifier: MIT
#pragma once

// Minimal Reaper API surface needed by Phase 0.
// Add REAPERAPI_WANT_<func> entries here as later phases require more.

#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_ShowConsoleMsg
// Spike: docked GL-window experiment
#define REAPERAPI_WANT_DockWindowAddEx
#define REAPERAPI_WANT_DockWindowActivate
#define REAPERAPI_WANT_DockWindowRemove
// Spike: PCM_source / transport-driven rendering experiment
#define REAPERAPI_WANT_GetPlayStateEx
#define REAPERAPI_WANT_GetPlayPosition2Ex
#define REAPERAPI_WANT_GetCursorPositionEx
#define REAPERAPI_WANT_CountMediaItems
#define REAPERAPI_WANT_GetMediaItem
#define REAPERAPI_WANT_GetMediaItemInfo_Value
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_GetMediaItemTake_Source

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
