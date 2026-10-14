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
    None, Pending, Reserved, Tsel, TselEdge, MkGap, MkZone, Swipe, Move, Scrub, Gain,
    Edge, Cut, GapEdge, Fade, Snap, Xf
};
// At the press: a handle drags at once (TselEdge, Edge, Cut, GapEdge, Fade, Snap, Xf); Ctrl+drag of anything and
// Alt+drag of a handle (but a Gap's crossfade, where Alt = one side) are reserved (nothing); else Pending.
ZoneDragKind PressDragKind(const ZoneHit& h, bool ctrl, bool alt);
// A Pending press moved sideways past 4 px (the mockup's startDrag): the ruler a time selection, Alt a Gap,
// Shift a new zone, a greyed zone a swipe, a zone's (or a Gap's) top half a move; else the cursor (scrub). The
// Variations and Placed strips: nothing.
ZoneDragKind StartZoneDrag(const ZoneHit& h, bool shift, bool alt);
// A Pending press on a part zone moved more vertically than sideways past 3 px (no Shift / Alt): its gain.
bool PendingBecomesGain(const SoundData& d, const ZoneHit& h, float dx, float dy, bool shift, bool alt);
// The gain a vertical drag of dy px gives (0.1 dB per px, up = louder).
double GainDragDb(float dy_px);

// The part a Shift+drag makes: Lane = the lane's part; Combined = `part_sel` (Settings of) when it is a part,
// else the part of the zone under the start, else the first part.
std::string NewZonePart(const SoundData& d, const ZoneHit& h, const std::string& part_sel);

// The ruler / new zone edges: t snapped to a zone edge or the cursor within `tol_px` (px_per_s = px per second).
double SnapZoneTime(const SoundData& d, double t, double px_per_s, double cursor, float tol_px);

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
enum class ZoneDoubleAction { None, RemoveTsel, TselVariation, TselZone, ResetEdge, ResetSnap, ResetFade };
ZoneDoubleAction DoubleActionOf(const ZoneHit& h);

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
std::vector<ZoneVarBlock> ZoneVarBlocks(const SoundData& d);
// "n · a–b ms" (the caller shows "n" alone when it does not fit).
std::string ZoneVarLabel(int n, double a, double b);
// Every zone of variation `frame` (time order); its first part zone (or zone).
ZoneSel ZonesOfFrame(const SoundData& d, int frame);
ZoneRef FirstZoneOf(const SoundData& d, int frame);
// The frames a selection touches (once each, ascending).
std::vector<int> SelFrames(const ZoneSel& sel);

// Selection helpers (view state: refs into the Sound's zones).
bool    SelHas(const ZoneSel& sel, const ZoneRef& r);
// Ctrl / Shift+click: adds r, or takes it out.
void    SelToggle(ZoneSel& sel, const ZoneRef& r);
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
