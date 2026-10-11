// SPDX-License-Identifier: MIT
//
// Epic 13 (D20, story 13-2) -- what the Tagging panel's sound sections (13-8, 13-10) see of the
// panel: the shown animation item, the panel's model, its rules and the selected rule.
// FROZEN after 13-2.

#pragma once

class MediaItem;  // REAPER SDK (reaper_plugin.h)

namespace rav {

struct TaggingModel;
struct ItemRules;

struct TaggingSoundContext {
    MediaItem*          item = nullptr;   // the shown animation item (null = none)
    const TaggingModel* model = nullptr;
    const ItemRules*    rules = nullptr;  // its rules as shown
    int                 rule = -1;        // the selected rule (index into rules->blocks), -1 = none
};

}  // namespace rav
