// SPDX-License-Identifier: MIT
//
// Build features. RAV_WITH_SOUNDS (CMake option, ON by default) builds RAV with the sound library
// (Epic 13). OFF, every sound entry point is off (views, panels, actions, options, timers) while all
// the sound code still compiles: both builds compile the same code. Gates read kWithSounds at the
// entry points (`if constexpr` / `if`), never deep in the sound code.
#pragma once

#ifndef RAV_WITH_SOUNDS
#define RAV_WITH_SOUNDS 1
#endif

namespace rav {

constexpr bool kWithSounds = RAV_WITH_SOUNDS != 0;

}  // namespace rav
