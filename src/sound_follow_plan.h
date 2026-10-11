// SPDX-License-Identifier: MIT
//
// Follow plan (13-9, pure): how placed sounds follow their animation item's events.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-9
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

// An event of the animation item now (rule id, clip time, project time).
struct FollowEvent {
    std::string rid;
    double      t = 0.0;
    double      project_t = 0.0;
};

enum class FollowKind { Move, Remove, Detach, Forget };

struct FollowAction {
    FollowKind  kind = FollowKind::Move;
    std::string item_guid;
    double      project_t = 0.0;  // Move: where it goes
};

struct FollowPlan {
    std::vector<FollowAction> actions;
    bool                      record_changed = false;
};

// What the placements of `rec` must do for these events; `present` = the placed GUIDs still in
// the project.
FollowPlan PlanFollow(const ItemSounds& rec, const std::vector<FollowEvent>& events,
                      const std::vector<std::string>& present);

}  // namespace rav
