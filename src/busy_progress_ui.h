// SPDX-License-Identifier: MIT
//
// 13-59 (Antho 2026-10-14: "every action that freezes the tool while it processes should show a
// loading bar"): the shared busy card of a long gesture. A gesture opens a BusyScope; it and the
// code it runs say where they are (BusyScope::Item, BusyStep); once the gesture has taken ~150 ms
// (busy_progress.h), RAV's window shows a card with the gesture's title, a bar and "Item i of n ·
// event j of m", repainted about every 100 ms, until the gesture ends. A quicker gesture shows
// nothing. The gesture itself is unchanged (still one undo point): only the card is painted, by the
// viewer (SetBusyPainter), from inside the gesture. Scopes nest: the outermost one names the card,
// the outermost one that counts items counts them, and BusyStep always sets the innermost count.

#pragma once

#ifdef _WIN32

#include "busy_progress.h"

namespace rav {

// The viewer's painter (viewer_window.cpp): `paint` draws one frame of the card on RAV's window and
// says whether it did (false when the window is hidden or not ready); `ended` runs once a gesture
// whose card was really painted is over (the clicks and keys made on the card are dropped). nullptr =
// no card (the viewer closed).
using BusyPaintFn = bool (*)(const BusyFrame& frame);
using BusyEndedFn = void (*)();
void SetBusyPainter(BusyPaintFn paint, BusyEndedFn ended);

class BusyScope {
public:
    // `eager`: a gesture that cannot say where it is during its long step (Detect's search, Analyse)
    // shows its card at once when its last run took long (busy_progress.h BusyMemory).
    explicit BusyScope(const char* title, bool eager = false);
    ~BusyScope();
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;

    // The item worked on (0-based) of `n`; its steps start again. A nested scope counts the items only
    // while no outer scope does (Repopulate counting its items, then Populate's own are ignored).
    void Item(int i, int n, const char* unit = "Item");
    // A short stage shown before the counts ("" = none; the outermost scope only).
    void Phase(const char* phase);

private:
    int level_ = 0;  // 1 = the outermost scope
};

// The steps of the current item done so far, of `total` (Populate: its events). Nothing without a
// running BusyScope. A frame is painted when one is due.
void BusyStep(int done, int total, const char* unit = "event");
// True while a BusyScope runs.
bool BusyActive();
// Review M4: names the running top-level gesture (changes each time an outermost BusyScope opens).
unsigned long long BusyGestureId();
// True once after a gesture that showed its card (then false until the next one): the viewer's frame
// that gesture ran in is not presented (its back buffer holds the card, not the frame).
bool BusyTakePainted();

// The card's bar as an ImGui widget (the Locate scan, Normalize's measuring): the accent fill on the
// theme background, `height_dp` high, the full width. `fraction` < 0 = indeterminate (animated).
void DrawBusyBar(float fraction, float height_dp = 6.0f);

}  // namespace rav

#endif  // _WIN32
