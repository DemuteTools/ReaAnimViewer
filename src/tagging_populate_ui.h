// SPDX-License-Identifier: MIT
//
// The Tagging panel's sound sections (13-8): rule library, item values, event sounds, the Populate row.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-8
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#ifdef _WIN32

#include <functional>
#include <string>
#include <vector>

#include "event_list.h"
#include "rule_record.h"  // ItemRules
#include "sound_model.h"   // SoundReport
#include "sound_populate.h"  // 13-19: PopulateProgressFn
#include "tagging_sound_context.h"

struct ImDrawList;

namespace rav {

// Under the selected rule's settings (13-15b): its sound as one summary line and an Assign... button that opens the
// Sound assignment window (tagging_assign_ui.h) on the rule (both sides of its marker pair).
void DrawRuleSoundSection(const TaggingSoundContext& ctx);
// In the Smart Detection section, under a marker type's row (or one of its per-side rows): the sound of these auto
// marker keys (sound_auto_assign.h) as a summary line and Assign.... With no block of those keys on the item yet
// the line shows what a new one would get and Assign... is off ("Detect first, or set it at Project / Character level").
void DrawAutoSoundRow(MediaItem* item, const ItemRules& rules, const std::vector<std::string>& keys);
// At the end of the event inspector: the event's sounds, overrides and draws.
void DrawEventSoundSection(const TaggingSoundContext& ctx, const ShownEvent& e);
// Antho 2026-10-12 (13-8 fix): the sound gestures the menus above picked this frame, run once the
// panel's widgets are drawn (a gesture may replace the sound store those menus list). Each is one
// undo point, as before.
void RunPickedSoundGestures();
// 13-15b: queues a gesture picked in the assignment window with the same rule: it holds its own copies and
// runs after the widgets (RunPickedSoundGestures), its report remembered like the menus' ones.
void PickSoundGesture(std::function<SoundReport()> gesture);
// True when the footer shows Populate (D9: Commit is then a SolidButton).
bool SoundFooterVisible(const TaggingSoundContext& ctx);
// The extra footer height (px) the sound rows take, 0 when hidden.
float SoundFooterHeight(const TaggingSoundContext& ctx);
// Populate (the one primary button), Reshuffle and Repopulate (story 10-10f: moved from Options > Sounds; its
// orange confirm bubble opens over it), leaving `attach_w` px on its right for Attach. Narrow, Populate's label
// shortens to "Populate · 9".
void DrawPopulateRow(const TaggingSoundContext& ctx, float attach_w);
// Story 10-10f: one part of the Tagging footer's status line ("1 selected", "preview not committed", "not
// populated yet"...): `col` its ink (an ImU32), `priority` how long it stays when the line is short (higher =
// longer), `click` set = a link (run when clicked), `tip` its own tooltip.
struct FooterSegment {
    std::string           text;
    unsigned int          col = 0;
    int                   priority = 0;
    std::function<void()> click;
    std::string           tip;
};
// The sound part of that line: "not populated yet" or the last Populate's report (a link to its details), and
// "Restore N" (a link) while sounds deleted by hand wait. Nothing while the sound footer is hidden.
void SoundStatusSegments(const TaggingSoundContext& ctx, std::vector<FooterSegment>* out);
// After the status line, in the same window: the sounds' (?) (the strip glyphs' legend) and the report's details
// popup (opened by its segment).
void DrawSoundHelpMark(const TaggingSoundContext& ctx);
void DrawSoundReportPopup();
// 13-19: the Populate button and the report line's Restore only queue their run (it cannot draw from inside
// the ImGui frame). RAV's window procedure runs it (RunPendingPopulate), outside any frame, with a painter
// for its progress frames (one undo point, as before); the pointers are checked then. Dropped with the viewer.
bool PopulateHasPending();
void RunPendingPopulate(const PopulateProgressFn& progress);
void DropPendingPopulate();
// 13-26 (Clear item data): the shown item's sound record, the Populate count and the last report are
// forgotten (read again on the next frame; the report line goes: it spoke of sounds now removed).
void DropSoundFooterCaches();
// On the strip, under an event's mark at (x, y): its sound glyph.
void DrawSoundStripGlyph(ImDrawList* dl, const TaggingModel& m, const ShownEvent& e, float x, float y);

}  // namespace rav

#endif  // _WIN32
