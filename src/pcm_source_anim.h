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

#include "reaper_api.h"  // pcmsrc_register_t, PCM_source (SDK types via reaper_plugin.h)

namespace rav {

// Address of the single static pcmsrc_register_t. plugin_main passes the SAME
// pointer to Register("pcmsrc", …) at load and ("-pcmsrc", …) at unload (AC3).
pcmsrc_register_t* PcmSourceRegistration();

}  // namespace rav
