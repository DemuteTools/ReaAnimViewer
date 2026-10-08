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

// Story 11-2 -- the video FX timeline snapshot (src/video_timeline.cpp), built on the
// main thread: the open projects and their change counter, the tracks carrying the FX
// and their items, the project video size (Project Settings config vars), and the
// per-track background option (P_EXT). All present in REAPER 7. (Kept here, before the
// 11-1 block, so later stories appending at the end do not collide.)
#define REAPERAPI_WANT_EnumProjects
#define REAPERAPI_WANT_GetProjectStateChangeCount
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_CountTrackMediaItems
#define REAPERAPI_WANT_GetTrackMediaItem
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_projectconfig_var_getoffs
#define REAPERAPI_WANT_projectconfig_var_addr

// Self-update from the Demute Reaper Toolkit copy (src/self_update.cpp): locate
// UserPlugins / the Toolkit folder, and tell the user to restart after a startup swap.
#define REAPERAPI_WANT_GetResourcePath
#define REAPERAPI_WANT_ShowMessageBox

// Story 11-1 -- the video FX: "RAV: Add video FX to selected track" finds or inserts
// rav_video_fx.clap on the selected tracks (src/video_fx_track.cpp). All present in
// REAPER 7 (REAPERAPI_LoadAPI fails the whole load if one is missing).
#define REAPERAPI_WANT_CountSelectedTracks
#define REAPERAPI_WANT_GetSelectedTrack
#define REAPERAPI_WANT_TrackFX_GetCount
#define REAPERAPI_WANT_TrackFX_AddByName
#define REAPERAPI_WANT_TrackFX_Delete
#define REAPERAPI_WANT_TrackFX_GetFXName
#define REAPERAPI_WANT_TrackFX_GetNamedConfigParm
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2

// Story 11-3 -- the video camera as FX parameters: shots are envelope points on the
// FX's six camera parameters, names / override / saved angles go through the FX state
// ("clap_chunk"), src/video_shots.cpp. All present in REAPER 7.
#define REAPERAPI_WANT_GetFXEnvelope
#define REAPERAPI_WANT_CountEnvelopePoints
#define REAPERAPI_WANT_GetEnvelopePoint
#define REAPERAPI_WANT_SetEnvelopePoint
#define REAPERAPI_WANT_InsertEnvelopePoint
#define REAPERAPI_WANT_DeleteEnvelopePointRange
#define REAPERAPI_WANT_Envelope_SortPoints
#define REAPERAPI_WANT_Envelope_Evaluate
#define REAPERAPI_WANT_GetEnvelopeScalingMode
#define REAPERAPI_WANT_ScaleToEnvelopeMode
#define REAPERAPI_WANT_ScaleFromEnvelopeMode
#define REAPERAPI_WANT_TrackFX_SetNamedConfigParm
#define REAPERAPI_WANT_TrackFX_GetParamNormalized
#define REAPERAPI_WANT_TrackFX_SetParamNormalized
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_GetUserInputs

// Story 11-4 -- Video view and the Video panel (src/video_view.cpp): the FX state on
// the track (bypassed / offline and their fix), the pinned track's validity, the
// project frame rate shown above the frame. All present in REAPER 7.
#define REAPERAPI_WANT_TrackFX_GetEnabled
#define REAPERAPI_WANT_TrackFX_SetEnabled
#define REAPERAPI_WANT_TrackFX_GetOffline
#define REAPERAPI_WANT_TrackFX_SetOffline
#define REAPERAPI_WANT_ValidatePtr2
#define REAPERAPI_WANT_TimeMap_curFrameRate

// Story 11-5 -- the shot strip moves REAPER's playhead; the Video panel's render buttons
// run REAPER's own actions (Region Render Matrix window, Render dialog). Core functions
// present in every REAPER 7. The action-name lookups (kbd_getTextFromCmd, ...) are NOT
// wanted here: src/reaper_actions.cpp resolves them optionally through GetFunc.
#define REAPERAPI_WANT_SetEditCurPos
#define REAPERAPI_WANT_Main_OnCommand
#define REAPERAPI_WANT_GetToggleCommandState

// Spec 11-fb-4 -- the REAPER catch-up readout turns project time into a delay: the
// loop range when the project repeats, and the playrate. Core functions, read-only.
#define REAPERAPI_WANT_GetSetRepeat
#define REAPERAPI_WANT_GetSet_LoopTimeRange2
#define REAPERAPI_WANT_Master_GetPlayRate

// Spec 11-fb-3 -- the viewer's custom keys (src/shortcuts.cpp), kept per machine in
// REAPER ExtState (section ReaAnimViewer, key shortcut.<id>). Present in every REAPER 7.
#define REAPERAPI_WANT_GetExtState
#define REAPERAPI_WANT_SetExtState
#define REAPERAPI_WANT_DeleteExtState

// Spec 11-fb-11 -- the Video panel's Envelopes button shows / hides the RAV video FX's
// envelopes in REAPER's arrange (src/video_shots.cpp). Present in every REAPER 7.
#define REAPERAPI_WANT_TrackFX_GetNumParams
#define REAPERAPI_WANT_GetSetEnvelopeInfo_String
#define REAPERAPI_WANT_TrackList_AdjustWindows

// Spec 11-fb-13 -- Render / Matrix first write the Output size into the project's video
// render format (src/video_view.cpp). Present in every REAPER 7.
#define REAPERAPI_WANT_GetSetProjectInfo_String
#define REAPERAPI_WANT_MarkProjectDirty
#define REAPERAPI_WANT_Undo_OnStateChangeEx

// Spec 11-fb-16 -- Render Current saves, sets and restores the render bounds and source
// (RENDER_BOUNDSFLAG / RENDER_STARTPOS / RENDER_ENDPOS / RENDER_SETTINGS).
#define REAPERAPI_WANT_GetSetProjectInfo

// Story 10-0 -- "RAV: Measure detection against reference markers" (src/detection_measure.cpp):
// the selected items, and one undo point for its RAV? take markers. Old, safe functions
// (GetUserInputs and UpdateArrange are wanted above). The take-marker functions
// (GetNumTakeMarkers, GetTakeMarker, SetTakeMarker, DeleteTakeMarker, REAPER 5.981+) are
// NOT wanted here: detection_measure.cpp resolves them optionally through GetFunc.
#define REAPERAPI_WANT_CountSelectedMediaItems
#define REAPERAPI_WANT_GetSelectedMediaItem
#define REAPERAPI_WANT_Undo_BeginBlock
#define REAPERAPI_WANT_Undo_EndBlock
// REF may also be project markers over the item (old, safe).
#define REAPERAPI_WANT_EnumProjectMarkers2

// Story 10-2 -- an item's auto-tagging rules live in its active take's P_EXT:RAV_RULES
// (src/item_rules.cpp). Old, safe (REAPER 5+).
#define REAPERAPI_WANT_GetSetMediaItemTakeInfo_String

// Story 10-3 -- the Tagging view names the item under the playhead (its take's name). Old, safe.
#define REAPERAPI_WANT_GetTakeName

// Story 10-4b -- the marker mirror (src/tag_markers.cpp): an item's GUID (its markers' owner),
// the RAV source's length (no asset loaded), one timeline redraw. Old, safe (REAPER 5+). The
// marker move (SetRegionOrMarkerInfo_Value, REAPER 7, fallback SetProjectMarkerByIndex2) is
// resolved optionally through GetFunc.
#define REAPERAPI_WANT_GetSetMediaItemInfo_String
#define REAPERAPI_WANT_GetMediaSourceLength
#define REAPERAPI_WANT_UpdateTimeline

// Story 10-6 -- pooled copies: "Make unique" gives an item a fresh pool id (a new GUID's text,
// src/item_rules.cpp). Old, safe (REAPER 4+).
#define REAPERAPI_WANT_genGuid
#define REAPERAPI_WANT_guidToString

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"
