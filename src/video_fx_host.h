// SPDX-License-Identifier: MIT
//
// Extension side of the video FX (Epic 11): the frame-render API that
// rav_video_fx.clap calls (src/video_fx_api.h), registered with REAPER at load and
// unregistered at unload. The API functions run on REAPER's video thread.
//
// Story 11-1: frames come from the test pattern (src/video_test_pattern.h) while the
// "RAV: Video FX test pattern" action is on; otherwise from the content path: the FX
// track's animation (Story 11-2, src/video_render.h). Registering the API also
// registers the main-thread timer that keeps the timeline snapshot (video_timeline.h).

#pragma once

namespace rav {

// plugin_register("API_RAV_GetVideoFxApi", ...). Call once the REAPER API is loaded.
bool RegisterVideoFxApi(int (*register_fn)(const char*, void*));

// Called first in the unload path: from then on every frame request answers "no
// picture" (a late video-thread call never runs into torn-down state), then the API
// is unregistered.
void UnregisterVideoFxApi(int (*register_fn)(const char*, void*));

// Story 11-4: non-zero when an FX instance asked for an API version other than ours
// (its rav_video_fx.clap and this extension come from different releases): that FX shows
// nothing until both are updated. Thread-safe.
int VideoFxApiMismatchedVersion();

// The test-pattern toggle (session only, off at startup). Thread-safe.
bool VideoTestPatternEnabled();
void SetVideoTestPatternEnabled(bool enabled);

}  // namespace rav
