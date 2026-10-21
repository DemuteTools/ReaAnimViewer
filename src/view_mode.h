// SPDX-License-Identifier: MIT
//
// The viewer's views and their cycle (V, the view toggle). Pure, host-tested (tests/view_mode_test.cpp);
// the current view lives in video_view.cpp (GetViewMode / SetViewMode).
#pragma once

#include "rav_features.h"  // release 0.4.0: kWithSounds

namespace rav {

// Story 10-3 -- the viewer's views, in the toggle's (and V's) order. Epic 13 (D20, 13-2): Library, last.
enum class ViewMode { Rav, Tagging, Video, Library };

// Release 0.4.0: the view a request for `mode` gets: without the sound library, Library is RAV view.
constexpr ViewMode ViewModeBuilt(ViewMode mode, bool with_sounds = kWithSounds)
{
    return !with_sounds && mode == ViewMode::Library ? ViewMode::Rav : mode;
}

// The next view in the cycle: RAV -> Tagging -> Video -> Library -> RAV; without the sound library
// (release 0.4.0) RAV -> Tagging -> Video -> RAV.
constexpr ViewMode NextViewMode(ViewMode mode, bool with_sounds = kWithSounds)
{
    return mode == ViewMode::Rav       ? ViewMode::Tagging
           : mode == ViewMode::Tagging ? ViewMode::Video
           : mode == ViewMode::Video   ? (with_sounds ? ViewMode::Library : ViewMode::Rav)
                                       : ViewMode::Rav;
}

}  // namespace rav
