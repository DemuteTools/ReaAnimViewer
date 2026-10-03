// SPDX-License-Identifier: MIT
//
// The frame-render ABI shared by the two binaries of Epic 11:
//   reaper_animviewer.dll  registers it with REAPER:  plugin_register("API_RAV_GetVideoFxApi", fn)
//   rav_video_fx.clap      finds it through REAPER:   rec->GetFunc("RAV_GetVideoFxApi")
//
// The CLAP is a thin shim. For every video frame REAPER asks it for, it fills a
// RavVideoFrameRequest, asks the extension for the frame size, allocates REAPER's
// frame, then asks the extension to draw into it. Everything the later stories need
// (camera parameters, track, FX identity, output override) is already in the request,
// so the ABI does not change when they land.
//
// Rules:
//  - Plain C types only; no REAPER, CLAP, STL or GL headers. Both sides are built from
//    this one header, by the same compiler, in the same repository.
//  - The version is checked exactly: a shim built for another version gets nullptr
//    from RAV_GetVideoFxApi and stays inert (no picture), it never guesses a layout.
//  - frame_size and render_frame are called on REAPER's video thread, possibly for
//    frames out of order (prebuffer, seeks, loops). A frame must be a pure function of
//    the request: project_time + the parameter values REAPER passes for that time.
//  - release_instance is called on REAPER's main thread when an FX instance is
//    destroyed, after REAPER has stopped asking it for frames.
//  - Nothing here throws across the boundary.

#pragma once

class ReaProject;
class MediaTrack;

// Bump on ANY change to the structs or function signatures below.
#define RAV_VIDEO_FX_API_VERSION 1

// Name passed to REAPER's GetFunc (and registered as "API_" RAV_VIDEO_FX_API_NAME).
#define RAV_VIDEO_FX_API_NAME "RAV_GetVideoFxApi"

// Identity of the plug-in, shared so the extension can find and insert it. Changing
// the CLAP id or the name breaks every project that already uses the FX.
#define RAV_VIDEO_FX_CLAP_ID "com.demute.reaanimviewer.video-fx"
#define RAV_VIDEO_FX_NAME    "RAV Video FX"
#define RAV_VIDEO_FX_VENDOR  "Demute"
#define RAV_VIDEO_FX_FILE    "rav_video_fx.clap"

struct RavVideoFrameRequest {
    int struct_size;            // sizeof(RavVideoFrameRequest), set by the shim

    // --- When -------------------------------------------------------------
    double project_time;        // seconds, the time REAPER asks the frame for
    double frame_rate;          // REAPER's frate (= project frame rate); <= 0 if unknown

    // --- Who --------------------------------------------------------------
    ReaProject* project;        // clap_get_reaper_context sel 3 (may be nullptr)
    MediaTrack* track;          // sel 1: the FX's parent track (nullptr for take FX)
    int fx_index;               // sel 6: index of this FX in its chain at this frame, -1 without a
                                // track. Best effort (REAPER cannot tell 0 from "unknown"): main-thread
                                // code finds the FX with FindVideoFxOnTrack (video_fx_track.h).
    void* fx_instance;          // opaque id of this FX instance, stable for its lifetime

    // --- Parameters, exactly as REAPER passed them for project_time ---------
    const double* parms;        // [0] = wet, [1..] = the plug-in's parameters, normalized 0..1
    int nparms;                 // = parameter count + 1; 0 when REAPER passed none

    // --- Output size chosen by the FX (its saved override), 0 = no override -
    int override_width;
    int override_height;

    // --- Destination, set by the shim between frame_size and render_frame ---
    int width;
    int height;
    unsigned char* pixels;      // REAPER 'RGBA' frame = LICE byte order B,G,R,A, top row first
    int row_bytes;              // bytes from one row to the next (>= width * 4)
};

struct RavVideoFxApi {
    int version;                // RAV_VIDEO_FX_API_VERSION of the extension
    int struct_size;            // sizeof(RavVideoFxApi) of the extension

    // Output size for this request. Writes 0 x 0 when there is no picture at
    // project_time (the shim then returns no frame and lower video tracks show).
    void (*frame_size)(const RavVideoFrameRequest* req, int* out_width, int* out_height);

    // Draws the picture into req->pixels (req->width x req->height). Returns 1 when a
    // picture was produced, 0 when not (the shim then releases the frame).
    int (*render_frame)(const RavVideoFrameRequest* req);

    // The FX instance req->fx_instance is gone: drop anything cached for it.
    void (*release_instance)(void* fx_instance);
};

// Signature of the registered function: returns the table when `version` equals
// RAV_VIDEO_FX_API_VERSION, nullptr otherwise.
typedef const RavVideoFxApi* (*RavGetVideoFxApiFn)(int version);
