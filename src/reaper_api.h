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

// Story 4.3 — the transport/item query that lets the viewer find the RAV item
// under the playhead and the time within it (resolved through the SAME existing
// REAPERAPI_LoadAPI call; read-only, no new registration — the Phase-3 additions
// architecture.md:337 lists are the first two, the rest are the item-walk to
// locate the current item). No other symbol is added (no Main_OnCommand / project
// helpers — those are later phases).
#define REAPERAPI_WANT_GetPlayStateEx          // transport playing? (bit 0)
#define REAPERAPI_WANT_GetPlayPosition2Ex      // play-cursor position (continuous audio clock)
#define REAPERAPI_WANT_GetCursorPositionEx     // edit-cursor position when stopped (scrub)
#define REAPERAPI_WANT_CountMediaItems
#define REAPERAPI_WANT_GetMediaItem
#define REAPERAPI_WANT_GetMediaItemInfo_Value  // "D_POSITION" / "D_LENGTH"
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_GetMediaItemTake_Source // the take's PCM_source* (confirm ours + read path)

// Story 4.4 — the take-level start-in-source offset (left-trim). Read via
// GetMediaItemTakeInfo_Value(MediaItem_Take*, "D_STARTOFFS") — NOT the item-level
// GetMediaItemInfo_Value (D_POSITION/D_LENGTH, already present). Resolves through
// the SAME existing REAPERAPI_LoadAPI call; read-only, no new registration.
#define REAPERAPI_WANT_GetMediaItemTakeInfo_Value  // "D_STARTOFFS" (take start-in-source — left-trim offset)

// Story 4.5 — multi-item current-item selection by track priority. When two RAV
// items span the playhead at once (overlap on different tracks), the topmost track
// wins (smallest 1-based IP_TRACKNUMBER). Read the spanning item's track, then its
// number. Both resolve through the SAME existing REAPERAPI_LoadAPI call; read-only,
// no new registration. IP_TRACKNUMBER is TRACK-level and returns the int directly.
#define REAPERAPI_WANT_GetMediaItem_Track          // item -> MediaTrack* (the spanning item's track)
#define REAPERAPI_WANT_GetMediaTrackInfo_Value     // "IP_TRACKNUMBER" (1-based, top=1 = highest priority; 0=not found, -1=master)

// Self-update from the Demute Reaper Toolkit copy (src/self_update.cpp): locate
// UserPlugins / the Toolkit folder, and tell the user to restart after a startup swap.
#define REAPERAPI_WANT_GetResourcePath
#define REAPERAPI_WANT_ShowMessageBox

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
