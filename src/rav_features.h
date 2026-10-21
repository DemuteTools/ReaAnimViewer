// SPDX-License-Identifier: MIT
//
// Build features. RAV_WITH_SOUNDS builds RAV with the sound library (Epic 13). Its value comes from
// CMake (CMakeLists.txt: the branch default, ON on dev, OFF on release; -DRAV_WITH_SOUNDS= overrides it);
// the fallback 1 below only serves targets built without the definition (the host tests). OFF, every sound entry point is off (views, panels, actions, options, timers) while all
// the sound code still compiles: both builds compile the same code. Gates read kWithSounds at the
// entry points (`if constexpr` / `if`), never deep in the sound code.
#pragma once

#ifndef RAV_WITH_SOUNDS
#define RAV_WITH_SOUNDS 1
#endif

namespace rav {

constexpr bool kWithSounds = RAV_WITH_SOUNDS != 0;

}  // namespace rav
