// SPDX-License-Identifier: MIT
//
// 13-62h (Lot E1): the Sound Edit window's waveform of a Sound's zones (13-62i: every Sound; an undivided one has
// one implicit part, no Lane view; a Takes-mode one is drawn one host per take, each on its own source) --
// maquette-zones-13-62h-v3.html in Dear ImGui. Combined and Lane draw and edit the same zones with the same
// gestures; Lane is the exploded view (one lane per part: its zones coloured, the others' greyed with a dashed
// edge, Gaps hatched on every lane) and adds comping (click a greyed zone / drag across it). Rows: the ruler (time
// selection), the Variations strip, in Lane the Placed strip, then the zones. The pure decisions are zone_hit.h.
//
// Writes: a drag = one Live draft re-applied from its start each frame (MoveZone, DragZoneEdge, ... on the drag's
// base refs) and one CommitLive on release (CancelLive when nothing changed); a click, a menu item or a key = one
// write (ZoneWaveHost::edit), probed first on a copy so nothing unchanged makes an undo point (a toast says why).
// View state (selection, time selection, Loop, the drag, the playing audition) lives in ZoneEditView (PanelState
// holds one): never saved, no undo point; ZoneEditReset clears it (OpenEdit).
//
// Helpers for Lot E2 (header, Selection panel, lane headers), each acting on the host's Sound with its toasts:
//   ZonePlayZone(h, z)        a zone's ▶ (a Gap: "Hear the jump"), the cursor at its start first
//   ZonePlaySelection(h)      the Selection panel's ▶ Play (one zone = ZonePlayZone; several = first start .. last chain end)
//   ZoneTransport(h)          ▶ Play / Space: from the edit cursor (time selection and Loop rules); again = stop
//   ZoneToggleLoop(h)         ⟳ Loop / its key (view state, toast)
//   ZoneDelete(h)             Delete: the selected zones removed, a selected Gap given to its left zone (a whole item
//                             selected: removed whole, empty space remains)
//   ZoneHeal(h)               H / 40548: heal the selected zones edge to edge (toast when they are not)
//   ZoneSplit(h)              B / 40757: split the zone under the edit cursor
//   ZoneTrimAt(h, side)       40511 (side < 0) / 40512 (> 0) at the cursor; ZoneTrimToTsel(h): 40508
//   ZoneGiveTo(h, part)       the selected part zones -> part ("Give them to")
//   ZoneSelectPart(h, part)   every zone of a part selected (double-click a lane header / part chip)
//   ZoneSnapToCursor(h)       40541: the selected zone's snap offset at the edit cursor (one undo point)
//   ZonePrevStop(h)           40172: the edit cursor back to the time selection / zone start before it, the file start last
//   ZoneStepVariation(h, dir) next (+1) / previous (-1) block of the Variations strip: its first zone, cursor at its
//                             start, view centred (same zoom)
//   ZoneClickPlays() / SetZoneClickPlays(on)   the Click plays pref (per user, ExtState, default on)
//   ZoneTailWins() / SetZoneTailWins(on)       Pre-roll / Tail overwrite hand edges (per user, default on)
//   ZoneEditView::sel / has_tsel, ts_a, ts_b / loop      the selection, the time selection, Loop
//   ZonePlaying(h)            whether the zone audition of this Sound plays

#pragma once

#ifdef _WIN32

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "sound_audition_plan.h"
#include "sound_model.h"
#include "sound_zones.h"
#include "zone_hit.h"

namespace rav {

// The drag in progress (view state).
struct ZoneDrag {
    ZoneDragKind kind = ZoneDragKind::None;
    ZoneHit      hit;           // at the press
    ImVec2       m0;            // the mouse at the press
    float        row_x = 0.0f;  // the row's left edge on screen (x -> t)
    int          row_index = -1;
    bool         shift = false, alt = false, ctrl = false, add = false;
    bool         moved = false;    // past the click slop
    bool         changed = false;  // the live draft differs from the store
    double       t0 = 0.0, t1 = 0.0;  // Tsel / MkZone / MkGap / Swipe: the span dragged
    ZoneSel      sel;           // Gain: the zones it moves (base refs); Move: the moved zone afterwards
    ZoneSel      after;         // the selection once the live gesture ran (re-resolved by it)
    double       value = 0.0;   // Gain: the delta (dB); Move: the delta (s)
    std::string  part;          // MkZone / Swipe: the part made
    int          tl = -1;       // 13-62i: the host (a Takes-mode Sound's take) it was pressed in: only that one runs it
};

// 13-67a: a right-button press on the waveform: a drag from empty space past 4 px is a marquee (the zones it touches
// selected, Ctrl adds), else the release opens the right-click menu where it was pressed (view state).
struct ZoneRightPress {
    bool    on = false;
    bool    empty = false;   // pressed on empty space of a zone row: may become a marquee
    bool    marquee = false;  // past 4 px from empty space
    bool    add = false;      // Ctrl at the press
    ImVec2  m0;
    ZoneHit hit;              // at the press (the menu's target)
    int     tl = -1;          // the host (a Takes-mode Sound's take) it was pressed in
};

struct ZoneEditView {
    ZoneSel     sel;  // the selected zones
    bool        has_tsel = false;
    double      ts_a = 0.0, ts_b = 0.0;
    bool        loop = false;
    ZoneDrag    drag;
    ZoneRightPress rpress;  // 13-67a
    ZoneTap     tap;
    // The hover (for the drawn hints), the right-click menu's target.
    bool        hover_on = false;
    ZoneHit     hover;
    int         menu_kind = 0;  // 0 empty space, 1 a zone, 2 a Gap, 3 an item (zones joined by Gaps, 13-62h v5)
    ZoneRef     menu_z;
    // The zone audition playing (tag "zone:<uid>"): its transport and heard map (the playheads).
    bool          playing = false;
    std::string   play_uid;
    EditTransport tp;
    HeardMap      map;
    double        last_pos = 0.0;  // its heard position last seen
    double        last_seen_t = 0.0;  // 13-67a: when (ImGui time) it was last seen playing
    // The stored Sound as last seen (its store generation): a change we did not write (Ctrl+Z, another panel)
    // carries the selection over by time and part (SelFollow), and drops the menu's target and the hover.
    SoundData     seen;
    std::string   seen_uid;
    uint64_t      seen_gen = 0;
    bool          own_pending = false;  // one of our writes is on its way (it moves the generation)
    int           own_frame = 0;
    // The right-click menu: the frame it was last drawn open, a close asked; the frame Esc was last used.
    int           menu_frame = -1;
    bool          menu_close = false;
    int           esc_frame = -1;
    // 13-62i: a Takes-mode Sound's active take (the host the edit cursor, the time selection and the keys belong to;
    // -1 = none yet) and the take the zone audition plays.
    int           cur_tl = -1;
    int           play_tl = -1;
};

// Clears the view state (a new Sound shown).
void ZoneEditReset(ZoneEditView& v);

// What the zone waveform needs of its Sound and of the Edit window (library_node_ui.cpp fills it each frame).
struct ZoneWaveHost {
    ZoneEditView*            view = nullptr;
    const SoundLibrary*      lib = nullptr;
    const SoundNode*         node = nullptr;     // the stored Sound
    std::function<const SoundData&()> shown;    // as drawn now (a live draft included)
    std::string              path;               // the file (13-62i: a take's own in Takes mode)
    double                   file_len = 0.0;
    int                      tl = -1;            // 13-62i: the timeline it shows (a Takes-mode Sound's take; sound_zones.h)
    std::vector<std::string> names;              // per divide value
    std::vector<ImU32>       colors;
    double*                  cursor = nullptr;   // the edit cursor (< 0 = none)
    std::string*             part_sel = nullptr; // Settings of ("" = the Sound)
    const std::set<std::string>* mute = nullptr;
    const std::set<std::string>* solo = nullptr;
    double*                  view_start = nullptr;
    double*                  view_len = nullptr;  // 0 = the whole file
    float                    head_px = 160.0f;    // the Lane view's header column (logical px, LaneHeadPref)
    std::function<SoundData&()> live;             // Live(n)
    std::function<void(const std::string& name, const std::string& toast)> commit;  // CommitLive
    std::function<void()>    cancel;              // CancelLive
    std::function<bool()>    live_now;            // a live draft is being shown
    // One write (one undo point): f on the stored Sound's data.
    std::function<void(const std::string& name, std::function<bool(SoundData&)> f, const std::string& toast)> edit;
    std::function<bool()>    store_replaced;
    // 13-62h (Antho): the detection threshold line dragged (Silence mode): `db` live while dragged, `done` on the
    // release (one undo point, the variations re-detected as the Detection row's threshold does). Null = no line.
    std::function<void(double db, bool done)> threshold;
    // 13-62h (v5): the item menu's Normalize: the selection's part zones measured as NormGroups (one gain per part's
    // zones glued by Gaps), one undo point when measured (a toast while measuring / waiting). Null = no entry.
    std::function<void(const ZoneSel& sel)> normalize;
};

// What the waveform leaves for the caller: the lane header column (Lane), the canvas, a middle-drag's scroll.
struct ZoneWaveEvents {
    float  head_x = 0.0f, head_w = 0.0f;
    bool   head_narrow = false;
    float  lanes_y = 0.0f, lane_h = 0.0f;
    int    lanes = 0;
    ImVec2 min, max;      // the zone canvas (all rows)
    double scroll_s = 0.0;
};

// Draws the zone waveform `height` px tall at the cursor and runs its gestures and its right-click menu.
ZoneWaveEvents DrawZoneWave(const char* id, const ZoneWaveHost& h, float height);
// The Edit window's keys for a zoned Sound while it has the focus: Delete, Space; Esc returns true when it used
// it (the mockup's: the right-click menu closes, and on the same press the selection goes, else the time
// selection), else the caller's Esc runs (stop, close).
void ZoneEditKeys(const ZoneWaveHost& h, bool* esc_used);
// The requests of the Shortcuts rows on a zoned Sound (kShortcutEditSplit / TrimLeft / TrimRight / TrimZone,
// kShortcutEditHeal / kShortcutEditLoop, next / previous variation): one undo point, or a toast.
void ZoneEditRequest(const ZoneWaveHost& h, int shortcut_id);
// Each frame while Edit shows a zoned Sound: Loop restarts (C4), the playing state follows the audition.
void ZonePumpPlay(const ZoneWaveHost& h);

void ZonePlayZone(const ZoneWaveHost& h, const ZoneRef& z);
void ZonePlaySelection(const ZoneWaveHost& h);
void ZoneTransport(const ZoneWaveHost& h);
void ZoneToggleLoop(const ZoneWaveHost& h);
void ZoneDelete(const ZoneWaveHost& h);
void ZoneHeal(const ZoneWaveHost& h);
void ZoneSplit(const ZoneWaveHost& h);
void ZoneTrimAt(const ZoneWaveHost& h, int side);
void ZoneTrimToTsel(const ZoneWaveHost& h);
void ZoneGiveTo(const ZoneWaveHost& h, const std::string& part);
void ZoneSelectPart(const ZoneWaveHost& h, const std::string& part);
void ZoneStepVariation(const ZoneWaveHost& h, int dir);
void ZoneSnapToCursor(const ZoneWaveHost& h);  // 13-67a: REAPER 40541 on the selected zone (one undo point)
void ZonePrevStop(const ZoneWaveHost& h);      // 13-67a: REAPER 40172, the cursor back one stop (view state)
bool ZonePlaying(const ZoneWaveHost& h);

bool ZoneClickPlays();
void SetZoneClickPlays(bool on);
bool ZoneTailWins();
void SetZoneTailWins(bool on);
// 13-67b: the snap offset mode (Attack / Peak / Start) a new Sound gets (Options, the user's pref, default Peak); read
// once (LoadZoneNewSnapMode at load), pushed to SetNewSoundSyncMode (sound_model.h). Saved Sounds keep their own.
SyncMode ZoneNewSnapMode();
void     SetZoneNewSnapMode(SyncMode m);
void     LoadZoneNewSnapMode();

}  // namespace rav

#endif  // _WIN32
