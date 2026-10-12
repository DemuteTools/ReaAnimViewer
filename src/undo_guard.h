// SPDX-License-Identifier: MIT
//
// Whether a gesture's Undo button may still be offered (13-7a, 13-10): Undo runs REAPER's
// "Edit: Undo", which reverts REAPER's LAST undo point. So it is offered only while that last
// point is the gesture's own (same description) and the project has not changed since.
// Pure, no REAPER: the caller reads Undo_CanUndo2 and GetProjectStateChangeCount.

#pragma once

#include <cstring>
#include <string>

namespace rav {

// `last_undo_desc`: Undo_CanUndo2(proj) (null when nothing can be undone).
// `gesture_desc`: the description the gesture gave its undo point (empty = unknown: no Undo).
// `state_now` / `state_at_gesture`: GetProjectStateChangeCount now and right after the gesture
// (state_at_gesture < 0 = not read yet, so only the description is compared).
inline bool GestureUndoAvailable(const char* last_undo_desc, const std::string& gesture_desc, int state_now,
                                 int state_at_gesture)
{
    if (gesture_desc.empty() || !last_undo_desc) return false;
    if (std::strcmp(last_undo_desc, gesture_desc.c_str()) != 0) return false;
    return state_at_gesture < 0 || state_now == state_at_gesture;
}

}  // namespace rav
