// SPDX-License-Identifier: MIT
//
// The Tagging panel's sound sections (13-8): rule library, item values, event sounds, the Populate row.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-8
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

#include "event_list.h"
#include "rule_record.h"  // ItemRules
#include "tagging_sound_context.h"

struct ImDrawList;

namespace rav {

// Under the selected rule's settings: its library and values.
void DrawRuleSoundSection(const TaggingSoundContext& ctx);
// Antho 2026-10-12: in the Auto detection section, under a marker type's row (or one of its
// per-side rows): the sound of these auto marker keys (sound_auto_assign.h): Library menu, then one
// compact value menu per Switch Group the library uses, wrapping to the width. The item's rule
// when it has those markers, else the type's own (what Detect gives). Nothing without a library.
void DrawAutoSoundRow(MediaItem* item, const ItemRules& rules, const std::vector<std::string>& keys);
// Under the inspector: the item-level Switch values.
void DrawItemSoundValuesSection(const TaggingSoundContext& ctx);
// At the end of the event inspector: the event's sounds, overrides and draws.
void DrawEventSoundSection(const TaggingSoundContext& ctx, const ShownEvent& e);
// Antho 2026-10-12 (13-8 fix): the sound gestures the menus above picked this frame, run once the
// panel's widgets are drawn (a gesture may replace the sound store those menus list). Each is one
// undo point, as before.
void RunPickedSoundGestures();
// True when the footer shows Populate (D9: Commit is then a SolidButton).
bool SoundFooterVisible(const TaggingSoundContext& ctx);
// The extra footer height (px) the sound rows take, 0 when hidden.
float SoundFooterHeight(const TaggingSoundContext& ctx);
// Populate (the one primary button), leaving `attach_w` px on its right for Attach.
void DrawPopulateRow(const TaggingSoundContext& ctx, float attach_w);
// The last Populate's report, one line.
void DrawSoundReportLine(const TaggingSoundContext& ctx);
// On the strip, under an event's mark at (x, y): its sound glyph.
void DrawSoundStripGlyph(ImDrawList* dl, const TaggingModel& m, const ShownEvent& e, float x, float y);

}  // namespace rav

#endif  // _WIN32
