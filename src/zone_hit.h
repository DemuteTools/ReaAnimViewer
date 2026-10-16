// SPDX-License-Identifier: MIT
//
// 13-62h (Lot E1): the Sound Edit window's zone waveform, its pure decisions -- what the mouse is over (the
// mockup's hitAt), what a press becomes once it moves (startDrag), the part a Shift+drag makes (newZonePart),
// the ruler's snap (snapT), our own double-click (tapKey / isDouble), what a double-click does (onDouble), the
// tooltips (tipOf) and hover cursors (CURSORS), the Variations strip, and the selection kept across a write.
// maquette-zones-13-62h-v3.html is the reference. Pure (no ImGui, no REAPER), host-tested (tests/zone_hit_test.cpp);
// zone_wave_ui.cpp draws and runs the gestures.

#pragma once

#include <string>
#include <vector>

#include "sound_model.h"
#include "sound_zones.h"

namespace rav {

// The rows of the zone waveform: the ruler, the Variations strip, the Placed strip (Lane only), a lane.
enum class ZoneRow { Ruler, Vars, Placed, Lane };

// What a lane draws and hits of one zone (AllZones order), computed once per change of the Sound (ZoneLooks): a
// part zone's envelope (rise from lo over fin, fall to hi over fout: a Gap's crossfade, else its fades), its gain,
// its fades as drawn, its snap; a Gap's crossfade and its neighbours.
struct ZoneLook {
    ZoneRef ref;
    double  a = 0.0, b = 0.0;
    bool    gap = false;
    int     k = -1;  // its part's index
    double  lo = 0.0, hi = 0.0, fin = 0.0, fout = 0.0;
    double  gain = 1.0;
    bool    lg = false, rg = false;  // a Gap before / after it (GapBefore / GapAfter)
    ZoneRef left, right;             // a Gap: its neighbours (GapLeft / GapRight)
    int     li = -1, ri = -1;        // ... their index in the looks
    double  xl = 0.0, xr = 0.0;      // a Gap: its crossfade sides
    double  fi_s = 0.0, fo_s = 0.0;  // its fades as drawn (ZoneFade, s)
    double  snap = 0.0;              // ZoneSnap
    bool    snap_own = false;
    bool    butt_next = false;       // the next zone starts where it ends (Butted)
};
// Every zone's look from one AllZones list (neighbours by index; the same values as GapBefore / GapAfter /
// GapLeft / GapRight / ZoneFade / ZoneSnap). 13-62i: of timeline `tl` (AllZones(d, tl)).
std::vector<ZoneLook> ZoneLooks(const SoundData& d, const PeakView* peaks, int tl = -1);

struct ZoneVarBlock;

// What a row shows and where (pixels local to the row; `dp` = one logical pixel on screen).
struct ZoneHitView {
    double          v0 = 0.0, vl = 1.0;   // the view: source seconds
    float           w = 1.0f, h = 1.0f;   // the row's size (px)
    ZoneRow         row = ZoneRow::Lane;
    int             lane = -1;            // a Lane row: its part's index; -1 = Combined (every zone is its own)
    bool            has_tsel = false;
    double          ts_a = 0.0, ts_b = 0.0;
    double          file_len = 0.0;       // 0 = unknown
    const PeakView* peaks = nullptr;      // the snap tags' place (ZoneSnap)
    float           dp = 1.0f;
    int             tl = -1;                // 13-62i: the timeline shown (a Takes-mode Sound's take; sound_zones.h)
    // The caller's cached ZoneLooks / ZoneVarBlocks of the Sound (null: computed here).
    const std::vector<ZoneLook>*     looks = nullptr;
    const std::vector<ZoneVarBlock>* blocks = nullptr;
};

enum class ZoneHitKind { None, Ruler, TselEdge, Var, Pick, Xf, Fade, Snap, Cut, GapEdge, Edge, Zone, Grey };

struct ZoneHit {
    ZoneHitKind kind = ZoneHitKind::None;
    ZoneRow     row = ZoneRow::Lane;
    int         lane = -1;
    double      t = 0.0;     // source seconds under the mouse
    bool        top = true;  // the top half of the row
    ZoneRef     z, z2;       // the zone (Cut: the left one, z2 the right one)
    int         side = 0;    // Edge / GapEdge / Fade / Xf / TselEdge: < 0 start / in / left, > 0 end / out / right
    int         var = -1;    // Var: the frame
};

// Whether lane `lane` (-1 Combined) owns zone r (a Gap is everybody's).
bool ZoneMine(const SoundData& d, const ZoneRef& r, int lane);
// What is under (x, y) of a row (the mockup's hitAt): on a lane, in this order, a Gap's crossfade handle and a
// fade handle in the top band, a snap tag, a cut grab at the bottom, a Gap edge, a zone edge (not held by a Gap),
// then the zone (another lane's part zone: Grey).
ZoneHit HitZones(const SoundData& d, const ZoneHitView& v, float x, float y);

// What a press does once the mouse moves.
enum class ZoneDragKind {
    None, Pending, Reserved, Tsel, TselEdge, MkGap, MkZone, Swipe, Move, MoveChain, Scrub, Gain,
    Edge, Cut, GapEdge, Fade, Snap, Xf
};
// At the press: a handle drags at once (TselEdge, Edge, Cut, GapEdge, Fade, Snap, Xf); else Pending. With Ctrl,
// or Alt on a handle (but a Gap's crossfade, where Alt = one side), it is Pending too: released unmoved it is the
// click (Ctrl adds / removes), moved it is reserved (ZoneReservedOnMove: nothing).
ZoneDragKind PressDragKind(const ZoneHit& h, bool ctrl, bool alt);
// Whether a Pending press does nothing once it moves: Ctrl+drag of anything, Alt+drag of a handle but a crossfade.
bool ZoneReservedOnMove(const ZoneHit& h, bool ctrl, bool alt);
// A Pending press moved sideways past 4 px (the mockup's startDrag): the ruler a time selection, Alt a Gap,
// Shift a new zone, a greyed zone a swipe, a zone's (or a Gap's) top half a move; else the cursor (scrub). The
// Variations and Placed strips: nothing.
ZoneDragKind StartZoneDrag(const ZoneHit& h, bool shift, bool alt);
// 13-62h (v5, v6 rule A): the same, a Move of a part zone of an item (ItemOf) moving the whole item (MoveChain);
// a Gap of an item stays a Move (it slides alone inside the item).
ZoneDragKind StartZoneDrag(const SoundData& d, const ZoneHit& h, bool shift, bool alt);
// A Pending press on a part zone moved more vertically than sideways past 3 logical px (`dp` = one on screen; no
// Shift / Alt): its gain.
bool PendingBecomesGain(const SoundData& d, const ZoneHit& h, float dx, float dy, bool shift, bool alt, float dp = 1.0f);
// The gain a vertical drag of dy logical px gives (0.1 dB per px, up = louder).
double GainDragDb(float dy_px);
// 13-62h (v5): the part zones a vertical drag on zone r moves: the selection's when r is selected (as before), else
// all its item's (ItemOf), else r alone.
ZoneSel GainDragZones(const SoundData& d, const ZoneSel& sel, const ZoneRef& r);
// The single zone gain drag's toast (the mockup's): "Heel gain: +2.0 dB (+ Heel fader +1 dB = 2.0 dB)" -- the
// fader is what it has applied to the zone's own gain (never added on top).
std::string ZoneGainToast(const std::string& zone_name, const std::string& part_name, double db, double fader_db);

// The part a Shift+drag makes: Lane = the lane's part; Combined = `part_sel` (Settings of) when it is a part,
// else the part of the zone under the start (on timeline `tl`), else the first part. 13-62i: an undivided Sound's
// kWholePart.
std::string NewZonePart(const SoundData& d, const ZoneHit& h, const std::string& part_sel, int tl = -1);

// The ruler / new zone edges: t snapped to a zone edge (of timeline `tl`) or the cursor within `tol_px` (px_per_s =
// px per second).
double SnapZoneTime(const SoundData& d, double t, double px_per_s, double cursor, float tol_px, int tl = -1);

// Our own double-click (the mockup's isDouble: two presses on the same thing within 0.45 s and 6 px).
struct ZoneTap {
    bool        valid = false;
    std::string key;
    double      time = 0.0;
    float       x = 0.0f, y = 0.0f;
};
std::string ZoneTapKey(const ZoneHit& h);
bool        ZoneDoubleTap(ZoneTap& last, const std::string& key, double now, float x, float y, float tol_px = 6.0f,
                          double max_s = 0.45);

// What a double-click does (the mockup's onDouble).
enum class ZoneDoubleAction { None, RemoveTsel, TselVariation, TselZone, TselItem, ResetEdge, ResetSnap, ResetFade };
ZoneDoubleAction DoubleActionOf(const ZoneHit& h);
// 13-62h (v5): the same, a zone / Gap of an item (a lane's own or greyed one, not the Placed strip's block) = the
// item whole selected, a time selection over it and the cursor at its start (TselItem).
ZoneDoubleAction DoubleActionOf(const SoundData& d, const ZoneHit& h);

// The hover cursor of a hit (the mockup's CURSORS: a zone's top half grabs, its bottom half is a text cursor).
enum class ZoneCursor { Arrow, Hand, ResizeEW, Text, Grab };
ZoneCursor ZoneCursorOf(const ZoneHit& h);
// The tooltip of a hit, in the mockup's words (tipOf). `names`: the parts' names by divide value index.
std::string ZoneTip(const SoundData& d, const ZoneHit& h, const std::vector<std::string>& names, bool click_plays);
// A zone's name: its part's, or "Gap".
std::string ZoneName(const SoundData& d, const ZoneRef& r, const std::vector<std::string>& names);

// The Variations strip: one block per on variation with zones (its first zone's start, its last zone's end).
struct ZoneVarBlock {
    int    frame = -1;
    double a = 0.0, b = 0.0;
};
// 13-62i: `tl` >= 0: only timeline tl's (a Takes-mode Sound's take); < 0: every variation (a count).
std::vector<ZoneVarBlock> ZoneVarBlocks(const SoundData& d, int tl = -1);
// "n · a–b ms" (the caller shows "n" alone when it does not fit).
std::string ZoneVarLabel(int n, double a, double b);
// Every zone of variation `frame` (time order); its first part zone (or zone).
ZoneSel ZonesOfFrame(const SoundData& d, int frame);
ZoneRef FirstZoneOf(const SoundData& d, int frame);
// The frames a selection touches (once each, ascending).
std::vector<int> SelFrames(const ZoneSel& sel);
// Next (+1) / previous (-1) variation (the mockup's stepVariation, over the Variations strip's blocks): from the
// one holding `ref` (the selected zone's middle, else the edit cursor; < 0 = none), else the first one after it /
// the last one before it. The block's index, -1 when there is none that way.
int ZoneStepBlock(const std::vector<ZoneVarBlock>& blocks, double ref, int dir);
// The step's reference: the middle of the selection's first zone, else `cursor`.
double ZoneStepRef(const SoundData& d, const ZoneSel& sel, double cursor);
// How many cuts healing `sel` removes: each pair of selected zones edge to edge, a selected Gap counted once.
int HealCutCount(const SoundData& d, const ZoneSel& sel);

// Selection helpers (view state: refs into the Sound's zones).
bool    SelHas(const ZoneSel& sel, const ZoneRef& r);
// Ctrl / Shift+click: adds r, or takes it out.
void    SelToggle(ZoneSel& sel, const ZoneRef& r);
// 13-62h (v5): Ctrl / Shift+click on a member r of `item`: r selected = the whole item taken out, else every member
// added (once each).
void    SelToggleItem(ZoneSel& sel, const ZoneSel& item, const ZoneRef& r);
// 13-62h (v5): the item the selection is, whole and alone (its members, time order; the Selection panel's item
// card), else empty.
ZoneSel SelItem(const SoundData& d, const ZoneSel& sel);
// "Heel + Heel" (its part zones' names) and "glued by a Gap" / "glued by 2 Gaps".
std::string ItemNames(const SoundData& d, const ZoneSel& item, const std::vector<std::string>& names);
std::string ItemGlue(const SoundData& d, const ZoneSel& item);
// What is heard of an item: its extent's HeardPieces (Gaps jumped, crossfades overlapped), seconds.
double ItemHeard(const SoundData& d, const ZoneSel& item);
// Its extent: its first member's start, its last member's end (false when empty).
bool ItemEdges(const SoundData& d, const ZoneSel& item, double* a, double* b);
// Only the refs that still point at a zone (once each).
ZoneSel SelValid(const SoundData& d, const ZoneSel& sel);
// The selection carried from `before` to `after` (a write that gave no selection back): each zone kept where it
// still is (same kind and part, still overlapping), else the zone of the same kind and part overlapping it most;
// dropped when none.
ZoneSel SelFollow(const SoundData& before, const ZoneSel& sel, const SoundData& after);
// The selection in time order.
ZoneSel SelSorted(const SoundData& d, const ZoneSel& sel);

// "120–260 ms".
std::string ZoneRangeMs(double a, double b);
// "+1.5 dB" / "-3.0 dB".
std::string ZoneDbText(double db);

}  // namespace rav
