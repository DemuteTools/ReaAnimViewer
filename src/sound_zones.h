// SPDX-License-Identifier: MIT
//
// 13-62h (spec 13-62h Contract, decisions-13-62h-items-model.md "Validated in the mockup sessions"): a divided
// Sound's variations as rows of zones laid on the file. A part zone (Heel, Toe...) is the REAPER item Populate
// places; a Gap is a wormhole between two zones it touches (playback jumps it, its neighbours glued by a
// crossfade); empty space is no zone. Combined and Lane draw and edit the same zones with the same gestures.
//
// Pure (no REAPER), host-tested (tests/sound_zones_test.cpp). A 1:1 port of the validated mockup's model
// (maquette-zones-13-62h-v3.html: relayout, cutOut, makeGap, fillGap, splitAt, trimAt, healSelected, moveRange,
// fadeOf, env, snapOf, pieces, chainOf, normalize) on SoundData: each zone belongs to its variation
// (SoundFrame::zones, C1), the gestures see every zone of the Sound in time order. Times are absolute source
// seconds. Every gesture returns false when it changed nothing (no undo point) and runs RelayoutZones after.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "normalize_presets.h"  // NormMode
#include "sound_entries.h"      // NormRegion, PartField
#include "sound_model.h"

namespace rav {

// What the zone helpers read the file's audio from (the cached peaks of the Sound's file).
using PeakView = PeakData;

// The smallest zone (part or Gap): 4 ms.
constexpr double kMinZone = 0.004;
// Two edges within this are touching (the mockup's EPS).
constexpr double kZoneTouch = 0.0002;

// Where a gesture runs: the peaks (null = none: automatic edges keep their place) and the file's length (0 =
// unknown: no upper clamp).
struct ZoneContext {
    const PeakView* peaks = nullptr;
    double          file_len = 0.0;
};

// A zone: its variation and its index in that variation's zones.
struct ZoneRef {
    int frame = -1;
    int index = -1;
};
inline bool operator==(const ZoneRef& x, const ZoneRef& y) { return x.frame == y.frame && x.index == y.index; }
inline bool operator!=(const ZoneRef& x, const ZoneRef& y) { return !(x == y); }

// True when the Sound is edited as zones: divided and not in Takes mode.
bool UsesZones(const SoundData& d);
// The zone `r` points at, nullptr when none.
const EditZone* ZoneOf(const SoundData& d, const ZoneRef& r);
EditZone*       ZoneOf(SoundData& d, const ZoneRef& r);
// Every zone of the Sound in time order (a Gap after a part zone starting at the same time).
std::vector<ZoneRef> AllZones(const SoundData& d);
// Counts: part zones, Gaps.
int PartZoneCount(const SoundData& d);
int GapCount(const SoundData& d);
// Every part zone of `part` ("" = every part zone), in time order.
std::vector<ZoneRef> ZonesOfPart(const SoundData& d, const std::string& part);

// "" when the invariant holds, else what breaks it: zones of a Sound never overlap; every Gap touches a part zone
// of its own variation on both sides; every part zone and Gap is >= 4 ms; every zone inside [0, file_len]
// (file_len <= 0: not checked above); each variation's zones in time order, its start / length = their extent;
// a part zone's part is a divide value; a Gap's crossfade sides >= 0 and each <= half its length.
std::string ZonesProblem(const SoundData& d, double file_len);

// ---- Layout -----------------------------------------------------------------------------------------------
// The mockup's relayout: RAV's cuts follow the right part's Pre-roll (before its onset); automatic edges follow
// their part's Pre-roll / Tail around the sound detected in their slot (with peaks; without, they stay); a Gap
// holds its neighbours' edges (one that no longer touches a zone on a side is empty space: removed); zones never
// overlap; everything inside the file; a zone under 4 ms goes. Each variation's start / length follow its zones;
// a variation left with no zone is turned off.
void RelayoutZones(SoundData& d, const PeakView* peaks, double file_len = 0.0);
// The mockup's initZones for variation `v` (its zones replaced): one zone per part, cut at RAV's cuts (AutoCuts
// on the peaks, else an even split), each later part's cut following its Pre-roll before its onset.
void ProposeZones(SoundData& d, int v, const PeakView* peaks);
// Every on variation of a zoned Sound with no zone gets ProposeZones, then RelayoutZones. Nothing on other Sounds.
void EnsureZones(SoundData& d, const PeakView* peaks, double file_len = 0.0);
// 13-62h: a Sound read from an old project / .ravlib (divided, cuts / owners / Sound-level zones / part_db / frame
// db, and no zone yet): each variation's placed pieces (the old model's PartPieces) become its zones, gains carried
// (frame db, part_db, the part's gain), hand syncs as the zone's own snap, the frame's own fades on its edges. The
// old keys are cleared. False when nothing was migrated.
bool MigrateLegacyZones(SoundData& d);

// ---- Queries ----------------------------------------------------------------------------------------------
// The zone holding t (a <= t < b), {-1, -1} when none.
ZoneRef ZoneAt(const SoundData& d, double t);
ZoneRef PrevZone(const SoundData& d, const ZoneRef& r);
ZoneRef NextZone(const SoundData& d, const ZoneRef& r);
bool    Butted(const SoundData& d, const ZoneRef& x, const ZoneRef& y);
// The Gap touching the zone's start / end, {-1, -1} when none.
ZoneRef GapBefore(const SoundData& d, const ZoneRef& r);
ZoneRef GapAfter(const SoundData& d, const ZoneRef& r);
// The part zones Gaps glue together around `r` (a Gap: around its left neighbour), in time order.
std::vector<ZoneRef> ChainOf(const SoundData& d, const ZoneRef& r);
// A Gap's neighbours (the part zones it touches), {-1, -1} when missing.
ZoneRef GapLeft(const SoundData& d, const ZoneRef& gap);
ZoneRef GapRight(const SoundData& d, const ZoneRef& gap);

enum class FadeSrc { Own, Cut, Gap, Part, Sound };
struct ZoneFadeInfo {
    double  s = 0.0;  // seconds
    FadeSrc src = FadeSrc::Sound;
};
// A part zone's fade on `side` (< 0 = in, > 0 = out): at a Gap its crossfade (xl + xr), else its own, else "At
// cuts" against a butted neighbour, else its part's own, else the Sound's; never more than half its length.
ZoneFadeInfo ZoneFade(const SoundData& d, const ZoneRef& r, int side);
// A part zone's gain (0..1, the fades and Gap crossfades) at t; t may run into a Gap next to it (the crossfade).
double ZoneEnv(const SoundData& d, const ZoneRef& r, double t);
struct ZoneSnapInfo {
    double t = 0.0;
    bool   own = false;
};
// Where the event lands on a part zone (REAPER's snap offset): its own (clamped into it), else its part's Sync
// point + Offset measured in it (AutoSync on the peaks; without peaks: Attack = its onset when known, else its
// start), clamped into it.
ZoneSnapInfo ZoneSnap(const SoundData& d, const ZoneRef& r, const PeakView* peaks);

// What is heard over [A, B] (the mockup's pieces): the part zones in time order, each from its start (or its left
// Gap's end minus xr) to its end (or its right Gap's start plus xl); `xo` = the crossfade it overlaps the next by
// (its right Gap's xl + xr); empty space as silence pieces (z = {-1, -1}); Gaps jumped.
struct HeardPiece {
    double  a = 0.0, b = 0.0, xo = 0.0;
    ZoneRef z;
};
std::vector<HeardPiece> HeardPieces(const SoundData& d, double A, double B);
// A variation's edges: its first zone's start, its last zone's end (its frame's when it has no zone). False for a
// bad index.
bool VariationEdges(const SoundData& d, int v, double* a, double* b);
// The variation index of the zones nearest to t (inside one = distance 0); -1 when the Sound has no frame.
int NearestVariation(const SoundData& d, double t);

// ---- Gestures (false = nothing changed: no undo point) ----------------------------------------------------
// A selection: zones (re-resolved by the gesture into `sel` when given).
using ZoneSel = std::vector<ZoneRef>;

// Clears [x0, x1]: zones inside go, zones across are trimmed or split (a Gap partly covered keeps its pieces with
// their outer crossfade).
bool CutOut(SoundData& d, double x0, double x1, const ZoneContext& ctx = {});
// A Gap over [x0, x1] between the zones it then touches on both sides (same variation); without a zone on a side
// it is only a trim (empty space). `made` gets whether a Gap was made; `sel` the new Gap.
bool MakeGap(SoundData& d, double x0, double x1, const ZoneContext& ctx = {}, bool* made = nullptr, ZoneSel* sel = nullptr);
// A new zone of `part` over [x0, x1] (what was there cleared; inside a Gap: Gap | zone | Gap); it joins the
// variation nearest to x0. `sel` gets it.
bool NewZone(SoundData& d, const std::string& part, double x0, double x1, const ZoneContext& ctx = {}, ZoneSel* sel = nullptr);
// The Gap goes to the zone on its left (it extends over it).
bool FillGap(SoundData& d, const ZoneRef& gap, const ZoneContext& ctx = {});
// Splits the part zone under t into two butted zones (none within 4 ms of an edge). `sel` gets the right one.
bool SplitZoneAt(SoundData& d, double t, const ZoneContext& ctx = {}, ZoneSel* sel = nullptr);
// REAPER's trims at the edit cursor (40511 side < 0: left of it / 40512 side > 0: right of it): the selected part
// zones holding t, else the part zone under t.
bool TrimZonesAt(SoundData& d, const ZoneSel& sel, double t, int side, const ZoneContext& ctx = {});
// 40508 Trim to selected area: the selected part zones (else the part zones under [a, b]) trimmed to [a, b] ∩ zone.
bool TrimZonesTo(SoundData& d, const ZoneSel& sel, double a, double b, const ZoneContext& ctx = {});
// REAPER's 40548 Heal: the selected zones edge to edge become one (it keeps the first one's part; a selected Gap
// between them is absorbed). `sel` (in / out) becomes the healed zone. `mixed` gets whether parts differed.
bool HealZones(SoundData& d, ZoneSel& sel, const ZoneContext& ctx = {}, bool* mixed = nullptr);
// Delete: selected part zones removed (empty space), a selected Gap given to the zone on its left. `sel` emptied.
bool DeleteZones(SoundData& d, ZoneSel& sel, const ZoneContext& ctx = {});
// The selected part zones become `part` (Lane comping; Selection panel "Give them to").
bool GiveZonesPart(SoundData& d, const ZoneSel& sel, const std::string& part, const ZoneContext& ctx = {});
// How far zone r can move by `delta` (its touching neighbours following, a neighbour across empty space stopping
// it, the file's edges).
double MoveRange(const SoundData& d, const ZoneRef& r, double delta, double file_len = 0.0);
// Moves zone r by `delta` (clamped by MoveRange): both edges; a butted neighbour's edge follows (a Gap's always
// do). `sel` gets it.
bool MoveZone(SoundData& d, const ZoneRef& r, double delta, const ZoneContext& ctx = {}, ZoneSel* sel = nullptr);
// One edge of a part zone (side < 0 start, > 0 end) to t, alone: it stops on its neighbour (opening empty space
// backing off); by hand (▼). An edge held by a Gap: false (DragGapEdge).
bool DragZoneEdge(SoundData& d, const ZoneRef& r, int side, double t, const ZoneContext& ctx = {});
// The cut between part zone r and the part zone butted after it, to t: both edges.
bool DragCut(SoundData& d, const ZoneRef& r, double t, const ZoneContext& ctx = {});
// A Gap's edge (side < 0 start, > 0 end) to t: the zone beside it follows.
bool DragGapEdge(SoundData& d, const ZoneRef& gap, int side, double t, const ZoneContext& ctx = {});
// A Gap's crossfade handle (side < 0 the left one: xl = t - start; > 0 the right one: xr = end - t), each side
// <= half the Gap; `both`: the two sides together (linked; Alt = one side).
bool DragGapXfade(SoundData& d, const ZoneRef& gap, int side, double t, bool both, const ZoneContext& ctx = {});
// A Gap's sides set (s, each clamped into [0, half its length]); < 0 = keep.
bool SetGapXfade(SoundData& d, const ZoneRef& gap, double xl, double xr, const ZoneContext& ctx = {});
// A part zone's own fade (side < 0 in, > 0 out; s >= 0); a side held by a Gap: false.
bool SetZoneFade(SoundData& d, const ZoneRef& r, int side, double s, const ZoneContext& ctx = {});
// A part zone's own snap offset (clamped into it).
bool SetZoneSnap(SoundData& d, const ZoneRef& r, double t, const ZoneContext& ctx = {});
// A part zone's own gain (dB, -60..+24, 0.1 dB).
bool SetZoneDb(SoundData& d, const ZoneRef& r, double db, const ZoneContext& ctx = {});
// Several part zones' gains moved by the same `delta` from `base` (their gains when the gesture began; empty =
// their gains now): the vertical drag on a selection.
bool ShiftZonesDb(SoundData& d, const ZoneSel& sel, double delta, const std::vector<double>& base = {},
                  const ZoneContext& ctx = {});
// A part zone's start / end set by number (as DragZoneEdge).
bool SetZoneEdge(SoundData& d, const ZoneRef& r, int side, double t, const ZoneContext& ctx = {});
// Back to automatic (= double-click): an edge, a fade side, the snap offset.
bool ResetZoneEdge(SoundData& d, const ZoneRef& r, int side, const ZoneContext& ctx = {});
bool ResetZoneFade(SoundData& d, const ZoneRef& r, int side, const ZoneContext& ctx = {});
bool ResetZoneSnap(SoundData& d, const ZoneRef& r, const ZoneContext& ctx = {});

// The lane fader (C3, Antho 2026-10-14): not a gain of its own; it moves the gain of every zone of `part` by
// `delta` (they keep their differences) and shows what it has applied (PartFaderDb). Double-click = minus it.
bool   ShiftPartGain(SoundData& d, const std::string& part, double delta, const ZoneContext& ctx = {});
// The fader set to `value` (dB): ShiftPartGain by value - what it shows.
bool   SetPartFader(SoundData& d, const std::string& part, double value, const ZoneContext& ctx = {});
double PartFaderDb(const SoundData& d, const std::string& part);

// Decision 7: after `part`'s ("" = the Sound's: every part that does not own it) Pre-roll (field Preroll) or Tail
// (field Tail) changed: with `tail_wins` every edge of those parts dragged by hand follows it again (an edge held by
// a Gap never moves); off: they stay. Returns how many hand edges there were (touched or kept); RelayoutZones runs
// after when some were released.
int ApplyPreTailChange(SoundData& d, const std::string& part, bool tail_wins, PartField field, const ZoneContext& ctx = {});

// Normalize: the groups measured together and given one gain: zones of one part glued by Gaps (ChainOf, same part).
// From a selection (part zones of it), or every part zone of `part` ("" = every part zone of the Sound).
std::vector<std::vector<ZoneRef>> NormGroups(const SoundData& d, const ZoneSel& sel);
std::vector<std::vector<ZoneRef>> NormGroups(const SoundData& d, const std::string& part);
// One NormRegion per group (frame = its variation, part = its part, spans = its zones; mode / target = the
// part's, else the given Sound-level ones). `path` left to the caller.
std::vector<NormRegion> NormGroupRegions(const SoundData& d, const std::vector<std::vector<ZoneRef>>& groups,
                                         NormMode sound_mode, double sound_target);
// Writes each group's gain (adj_db[i] for groups[i]; NaN or > +60 dB = silent, skipped) into its zones' db
// (-60..+24, 0.1 dB); `reset_faders`: the faders of the parts touched back to 0 (nothing applied on top). Returns
// how many groups got a gain.
int ApplyZoneNormalize(SoundData& d, const std::vector<std::vector<ZoneRef>>& groups, const std::vector<double>& adj_db,
                       bool reset_faders);

}  // namespace rav
