// SPDX-License-Identifier: MIT
//
// The single PCM_source subclass (boundary rule, architecture.md:874): an
// animation file (.glb/.gltf/.fbx/.dae) dropped on a Reaper track becomes a
// media item backed by rav::AnimSource via the pcmsrc_register_t factory below.
//
// Story 4.1 scope = registration + a working source object ONLY. The source
// reports a PLACEHOLDER length (real duration = Story 4.2) and does NOT load the
// asset, touch GL, or map the playhead to a frame (= Story 4.3). Getting an item
// to appear at all on drop is the proof 4.1 works.
//
// plugin_main.cpp is the ONLY file that calls rec->Register (boundary rule,
// architecture.md:875), so it asks for the registration object here and owns the
// symmetric pcmsrc / -pcmsrc (un)register with the IDENTICAL pointer (NFR-R3).

#pragma once

#include <string>

#include "reaper_api.h"  // pcmsrc_register_t, PCM_source (SDK types via reaper_plugin.h)

namespace rav {

// Address of the single static pcmsrc_register_t. plugin_main passes the SAME
// pointer to Register("pcmsrc", …) at load and ("-pcmsrc", …) at unload (AC3).
pcmsrc_register_t* PcmSourceRegistration();

// Story 4.3 — the read-only transport→current-item query the viewer polls each
// render tick. Reads the playhead (play cursor while playing, edit cursor while
// stopped) and walks ALL the project's items, selecting the spanning RAV item on
// the HIGHEST-PRIORITY (topmost) track — smallest 1-based IP_TRACKNUMBER — so an
// overlap on different tracks resolves to the topmost track (Story 4.5, FR14).
// Ties (same track, or an unreadable track number) keep the first walk-order item
// (deterministic). On a match: fills out_path with that item's source file path and
// out_anim_time with the item-relative time (low-guarded ≥0; RenderFrame clamps the
// high end to [0, duration]), and returns true. No match → returns false, leaving
// both outs untouched.
//
// NO-THROW, MAIN-THREAD ONLY: it calls Reaper item APIs, so it must run on the
// thread Reaper drives the UI pump on (the viewer's NULL-hwnd render timer is
// dispatched there). It only READS Reaper state — it does not register anything
// (the boundary rule keeps rec->Register in plugin_main.cpp).
bool GetCurrentAnimItem(std::string& out_path, double& out_anim_time);

// Story 11-4 — the same query, optionally restricted to ONE track (Video view on a
// pinned track: the first spanning RAV item in that track's item order, the video FX's
// rule), and reporting the chosen item's track (`out_track` may be null; untouched when
// nothing matches). only_track == nullptr = GetCurrentAnimItem. Main thread, no-throw.
// Story 10-3: `out_item` (optional) gets the chosen item, untouched when nothing matches.
bool GetCurrentAnimItemOn(MediaTrack* only_track, std::string& out_path, double& out_anim_time,
                          MediaTrack** out_track, MediaItem** out_item = nullptr);

// Story 11-2 — the rules above, shared with the video FX timeline (video_timeline.cpp)
// so the rendered video maps time exactly like the viewer.
//
// True iff `s` (or the source it wraps: section/reverse take) is a RAV animation source.
// Null-safe. Main thread (it calls the source's virtuals).
bool IsRavAnimSource(PCM_source* s);

// Item-relative animation time for project time `pos`, given the item position and the
// take's start-in-source offset (D_STARTOFFS): (pos - item_pos) + start_offs, low-guarded
// at 0 (a NaN offset also gives 0). The renderer clamps the high end to the clip. Pure.
double ItemAnimTime(double pos, double item_pos, double start_offs);

}  // namespace rav
