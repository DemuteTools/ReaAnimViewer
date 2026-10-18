// SPDX-License-Identifier: MIT
//
// Sound entries (13-3): a Sound's variations (frames) from its takes, take-marker slices or silence.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-3
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "normalize_presets.h"  // NormScope, NormMode (13-28)
#include "sound_model.h"

namespace rav {

// A take marker of the entry's take, in source seconds.
struct TakeMarkerPos {
    double      pos = 0.0;
    std::string name;
};

// What detection reads of an entry (the REAPER side fills it; tests build it by hand).
struct EntryFacts {
    double                  source_len = 0.0;  // the active take's source length (s)
    double                  start_offs = 0.0;  // the active take's start in its source (s)
    double                  item_len = 0.0;    // the item's length (s)
    double                  playrate = 1.0;    // the active take's D_PLAYRATE (item s x playrate = source s)
    std::vector<SoundFrame> takes;             // one per take of the item (start = its start offset, db = its gain)
    std::vector<Slice>      slices;            // the active take's slices (take markers or .rpp text)
    double                  item_db = 0.0;     // the item's own volume (D_VOL, dB): every detected variation's gain
};

// Slices from an item's .rpp chunk text (the fallback reader of take markers, spike 13-0 A):
// region lines (flag &1) ignored, =START shifts zero, slices of <= 1 ms dropped. A slice of
// length -1 runs to the source end (no =END); DetectFrames resolves it.
std::vector<Slice> SlicesFromRppText(const std::string& item_chunk_text);
// Slices between consecutive take markers (the last one runs to source_len). Needs >= 2 markers.
std::vector<Slice> SlicesFromTakeMarkers(const std::vector<TakeMarkerPos>& markers, double source_len);
// Variations split on silence of the source's peaks.
std::vector<SoundFrame> SplitOnSilence(const PeakData& peaks, const SilenceParams& params, double source_len);
// The detected (pre-edit) frames of an entry for a mode. `peaks` may be null (no Silence mode then).
// The Whole fallback is the item's stretch inside [0, source_len]; {} when that is 1 ms or less.
// Lengths are in source seconds: the item's length times the playrate (13-24 review).
std::vector<SoundFrame> DetectFrames(const EntryFacts& facts, DetectMode mode, const SilenceParams& silence,
                                     const PeakData* peaks);
// Keeps the frames inside [0, source_len], drops empty ones, sorts by start.
void ClampFrames(std::vector<SoundFrame>& frames, double source_len);
// Back to the detected frames (hand edits dropped, `edited` cleared). 13-20/23/25 (A7): the hand
// sync points go to the new frame that holds them (CarryHandMarks without cuts), excluded zones stay,
// "-> value" zones and hand cuts go. The caller runs RefreshParts after (it has the peaks). 13-62: owners go too.
void ResetToDetected(SoundData& data, const std::vector<SoundFrame>& detected);
// 13-67b: the Sound's detection mode set to `mode`, `detected` its fresh variations. The variations left (their zones,
// hand edits and `edited` flag) are parked (SoundData::parked); back to the parked mode, those come back instead of
// `detected` (Silence -> Takes -> Silence keeps the hand edits). False when already in `mode`. The caller runs
// RefreshParts after.
bool SwitchDetectMode(SoundData& data, DetectMode mode, const std::vector<SoundFrame>& detected);
// The frame's peak (dBFS), -inf when the peaks do not cover it.
double FramePeakDb(const PeakData& peaks, const SoundFrame& frame);
// The peak (dBFS) over [start, start + length) source seconds, -inf when the peaks do not cover it.
double SpanPeakDb(const PeakData& peaks, double start, double length);

// The default mode of a new entry (DECISIONS Q24): Takes when >= 2 takes, else TakeMarkers when
// >= 2 slices, else Silence when peaks exist, else Whole.
DetectMode DefaultDetectMode(const EntryFacts& facts, bool has_peaks);

// A variation on / off (`hand` and `data.edited` set). 13-62h: turned on, its zones come back into the model
// (proposed when it has none) and are laid out with the others.
void SetFrameOn(SoundData& data, int i, bool on);

// ---- Fades (Antho 2026-10-12: fade in / out on each variation, against clicks) -------------------
// A frame's fades as heard and placed: its own (fade_in / fade_out >= 0) or else the Sound's default,
// never negative, and in + out never past the frame's length (both scaled down together when they
// would be). `own_*` tells whether the frame set it (a Whole-item copy keeps its item's fade when not).
struct FrameFades {
    double in = 0.0;
    double out = 0.0;
    int    in_shape = 0;
    int    out_shape = 0;
    bool   own_in = false;
    bool   own_out = false;
};
// Clamps a fade pair into a length: each >= 0, in + out <= length (scaled down together).
void ClampFadePair(double length, double* in, double* out);
FrameFades EffectiveFades(const SoundData& data, const SoundFrame& frame);
// The Sound's default fades (each >= 0, 0 = none).
void SetDefaultFades(SoundData& data, double in, double out);

// ---- 13-20 / 13-23 / 13-25: parts, zones, sync points (spec 13-20-23-25 D5 / D6) -----------------
// A Sound divided by a Switch Group (divide_group, divide_values: part k = value k, in time order) cuts
// each variation into one part per value at its `cuts`. Zones (later ones win) exclude a stretch of the
// source (`to` "") or give it to a part. Each part has a sync point (snap offset): the moment placed on
// the event. Times are absolute source seconds.

// A stretch of one variation and who owns it (a value uid; "" = the whole variation, combined).
struct PartSpan {
    std::string value_uid;
    double      start = 0.0;
    double      length = 0.0;
};

// True when the Sound is divided (a group and at least one value).
bool IsDivided(const SoundData& data);
// How many parts each variation has: the divide values' count, 1 when combined.
int PartCount(const SoundData& data);
// The index of a value in divide_values, -1 when absent. 13-62i: kWholePart is part 0 of an undivided Sound.
int PartIndex(const SoundData& data, const std::string& value_uid);
// 13-62i: the part keys of a Sound's zones, in order: its divide values, or {kWholePart} when undivided.
std::vector<std::string> PartKeys(const SoundData& data);
// Who owns source time `t` of variation `variation`: false when it is outside the frame or excluded
// (frame off, or the last zone over it excludes it); else `value` gets the owner: the last zone's `to`
// when it is a divide value, else the part by cuts (divided) or "" (combined).
// 13-62: the part by cuts is the piece's owner (SoundFrame::owners) when the frame has them (one per
// piece, each a divide value), else the legacy rule (piece k -> divide_values[min(k, n - 1)]).
bool PartAt(const SoundData& data, int variation, double t, std::string* value);
// Every non-excluded contiguous stretch of the variation with its owner, in time order (a part cut by
// an excluded zone gives 2 spans; an empty part gives none). {} for a bad index or an off frame.
std::vector<PartSpan> PartSpansOf(const SoundData& data, int variation);
// 13-54 (Antho 2026-10-14, "each part is a section inside the section"): true when a Sound's detection
// margins also apply to each part: Silence detection and divided. Combined Sounds: never.
bool PartMarginsApply(const SoundData& data);
// 13-54: the spans each part plays: PartSpansOf with the stored `part_trims` applied (when the margins
// apply and the trims fit the spans and their key, PartTrimsStale), else PartSpansOf. The trims are
// measured on the peaks by RefreshFrame / MeasureSyncs: the Sound's pre-roll and tail around each raw
// span's own sound (the bins at or above the threshold), at the INNER edges only (13-54 review): a cut
// or an excluded zone. The variation's own start and end stay where they are (a detected frame already
// carries the pre-roll and tail there; a hand-stretched one stays as stretched). Between two touching
// parts (13-58, Antho 2026-10-14): zones never overlap and never shrink each other, margins only fill
// free space. The earlier part keeps its full zone (its sound plus its tail, which may run on across the
// cut up to the later part's sound); the later part's pre-roll butts on it, getting what is left of the
// gap (possibly nothing). Next to an excluded
// zone a span never grows past its own edge. A span with no loud bin plays nothing when its part sounds
// elsewhere in the variation (dropped, its silence shared like a gap), else it is kept as it is (a part
// quiet all along still plays, a wall its neighbours stop at). Parts never overlap.
// 13-62: each span uses its own part's pre-roll and tail (PartPrerollMs / PartTailMs).
std::vector<PartSpan> PartPieces(const SoundData& data, int variation);
// What a placement of `part_value` plays: the pieces of that part (PartPieces: its margins applied), or
// for "" every non-excluded stretch of the frame (raw, adjacent spans merged: the whole variation minus
// its exclusions, value_uid "" -- a whole variation keeps its own margins).
std::vector<PartSpan> PlacedSpans(const SoundData& data, int variation, const std::string& part_value);
// 13-54 review: the key part trims are measured with (13-58: led by the margin rule's version, so trims
// measured by an older rule are stale): the Silence threshold, pre-roll and tail, the
// variation's raw spans (bounds to the microsecond and owners, hashed) and `file`, the version of the
// file the peaks were read from (date + size; zero = unknown). Stored as SoundFrame::part_trim_key.
// 13-62: the parts' own pre-roll / tail are in the key too (only when a part owns one: same key as before
// otherwise).
std::string PartTrimKey(const SoundData& data, int variation, const FileVersion& file);
// 13-54 review: true when the variation's trims must be measured again while the margins apply: missing,
// not fitting the spans, or measured with another threshold / pre-roll / tail / raw spans; with `file`
// (known, non-zero), also when they were measured on another version of the file (re-rendered at the
// same path). False when the margins do not apply.
bool PartTrimsStale(const SoundData& data, int variation, const FileVersion* file = nullptr);

// One onset of a variation: the peak bin's time, its level, and where its rise starts (the last
// minimum of the 15 ms before it).
struct Onset {
    double t = 0.0;
    double db = 0.0;
    double rise = 0.0;
};
// The onsets in [start, start + length): local maxima of the peaks, >= 20 ms apart (the louder kept),
// rising >= 6 dB over the minimum of the previous 15 ms. Time order.
std::vector<Onset> FindOnsets(const PeakData& peaks, double start, double length);
// RAV's cuts for `parts` parts of a frame: the parts - 1 loudest onsets after the first, by time, 5 ms
// before each rise (kept inside the frame, >= 1 ms apart); too few onsets: an even split. `unsure`: 2
// parts and the two loudest candidates within 3 dB.
std::vector<double> AutoCuts(const PeakData* peaks, const SoundFrame& frame, int parts, bool* unsure);
// The sync point of a set of spans (one part, or the whole variation): Attack = the first bin reaching
// max(threshold, the spans' peak - 20 dB), capped at the peak (a part quieter than the threshold still
// has an attack), Peak = the loudest bin, Slice start = the first span's start,
// then + the offset, clamped into a span. Attack / Peak without peaks (or silent spans): the first span's
// start + offset. -1 when `spans` is empty.
// 13-62: the mode and offset are the spans' part's (PartSyncMode / PartSyncOffsetMs of the first span's
// value_uid; "" = the Sound's).
// Fix 2026-10-19: with the file's samples (PeakData::samples) Attack / Peak land on the sample (Peak = the frame of
// the largest |sample|, Attack = the first frame reaching the level above), `*fine` = true; else on the bins (a bin's
// start: up to a bin early).
// Freeze audit #2: `samples` false = on the peak bins only (a live drag's frame), never a read of the file.
double AutoSync(const SoundData& data, const PeakData* peaks, const std::vector<PartSpan>& spans, bool* fine = nullptr,
                bool samples = true);
// Keeps a time inside the spans (the nearest span edge when it falls between or outside them).
double ClampIntoSpans(const std::vector<PartSpan>& spans, double t);
// Refreshes one variation's cuts and sync points (every write that changes frames, detection, divide,
// sync settings or zones runs RefreshParts). Cuts: hand ones kept while there are parts - 1 of them,
// else AutoCuts (no peaks: the stored ones while their count fits, else an even split); none when
// combined. 13-62: a variation with sane owners (one per piece, each a divide value, cuts in order) keeps its
// cuts, whatever their count; owners that do not fit are dropped. A cut within 4 ms of the frame's edge (a
// trim moved past it) goes with the piece it cut off there. Syncs: per part, a hand sync inside its spans wins, else AutoSync; -1 for a part with no
// span. Without peaks, Attack / Peak syncs that cannot be kept (a stored one still inside its part)
// leave `syncs` empty: measured at placement (SyncsStale). Takes mode: no cuts, no syncs (A8).
// 13-62i: every Sound is laid out as zones (RefreshFrame: that variation's timeline gets the peaks; RefreshParts:
// timeline `tl` does, sound_zones.h ZoneTimeline -- a Takes-mode Sound's peaks are one take's, -1 = none of them).
void RefreshFrame(SoundData& data, int variation, const PeakData* peaks);
void RefreshParts(SoundData& data, const PeakData* peaks, int tl = -1);
// True when the variation's syncs must be measured again: not one per part, or a part with spans whose
// sync is not inside them, or one without spans whose sync is set; 13-54: or its part trims are stale
// (PartTrimsStale without a file: the caller that knows the file checks it too).
bool SyncsStale(const SoundData& data, int variation);
// 13-25 (D6): a variation's sync points measured at placement (Populate found them stale): per part, a
// hand sync inside its spans, else AutoSync; its cuts are kept (Populate never re-detects). False (the
// syncs left as they were) for Takes mode, a bad index, or Attack / Peak without peaks.
bool MeasureSyncs(SoundData& data, int variation, const PeakData* peaks);
// 13-25 (D6): writes the syncs Populate measured (`measured`, a copy of the Sound's data) back into the
// store: Sound `node_uid` of library `lib_uid`, the given variations, each only while its frame still
// has the same start and length. True when a sync changed (false for an unknown library or node).
bool ApplyMeasuredSyncs(SoundStore& store, const std::string& lib_uid, const std::string& node_uid,
                        const SoundData& measured, const std::set<int>& variations);
// The sync point a placement of `part_value` uses: that part's; for "" the first part's that has one.
// -1 when none (empty part, or not measured).
double SyncFor(const SoundData& data, int variation, const std::string& part_value);
// Carries the hand marks of `old` frames onto `now` (after MergeHandFrames or a Reset): each hand sync
// goes to the new frame holding it; a frame's hand cuts follow when they all fall inside one new frame
// (only when `with_cuts`). The marks `now` held are replaced. 13-62: a frame's owners travel with its cuts (and are cleared
// with them); a frame whose owners hold no cut follows its middle. Several comped frames landing in one new
// frame give it all their pieces in time order, with a cut at each later old frame's start inside it; when
// one of them has hand cuts but no owners, the first one's cuts alone are kept (as before 13-62).
void CarryHandMarks(std::vector<SoundFrame>& now, const std::vector<SoundFrame>& old, bool with_cuts = true);

// ---- 13-73: Left / Right per variation (spec 13-73) -------------------------------------------------------------
// The side the Pattern gives the k-th variation (time order): 'L' or 'R'.
char PatternSide(SidePattern p, int k);
// The other side ('L' <-> 'R'; 0 stays 0).
char OtherSide(char side);
// The Pattern's name as the Parts card shows it ("Alternate L R", "Alternate R L", "All Left", "All Right").
const char* SidePatternLabel(SidePattern p);
// Each frame's rule side (no hand side), in frame order: its own `side` when set; else, for an alternating Pattern,
// the other side of the frame before it (the first one: the Pattern's first side), for All Left / All Right that side.
// So a variation that appeared (a re-detection, a new zone) continues the alternation of its neighbours.
std::vector<char> FrameRuleSides(const SoundData& d);
std::vector<char> FrameRuleSides(const std::vector<SoundFrame>& frames, SidePattern p);
// Each frame's side as played: its hand side when set, else its rule side.
std::vector<char> FrameSides(const SoundData& d);
// Whether frame `i`'s side was set by hand (a hand side that differs from its rule side).
bool FrameSideByHand(const SoundData& d, const std::vector<char>& rule, int i);
// Every frame's rule side written (what FrameRuleSides gives now, so deleting or adding a variation never shifts the
// others); a hand side equal to it cleared. True when something changed. 13-73 review: run before every zone edit.
bool LockSides(SoundData& d);
// Left / Right on or off. On: every frame's rule side is written (locked, so a later re-detection keeps it). False when
// nothing changed.
bool SetSidesOn(SoundData& d, bool on);
// A new Pattern: every frame's rule side from it, in time order; the hand sides stay (one now equal to its rule side is
// no longer by hand: cleared). False when nothing changed.
bool SetSidePattern(SoundData& d, SidePattern p);
// Swap L/R: the Pattern to its mirror (L R <-> R L, All Left <-> All Right), every rule side and hand side flipped.
bool SwapSides(SoundData& d);
// The frames `frames` set to `side` by hand ('L' / 'R'), or back to the rule (side 0). A side equal to the rule's is
// not "by hand" (cleared). False when nothing changed.
bool SetFrameSides(SoundData& d, const std::vector<int>& frames, char side);
// Every hand side cleared (Reset hand sides). False when there was none.
bool ResetHandSides(SoundData& d);
// How many frames play each side and how many were set by hand.
struct SideCounts {
    int left = 0, right = 0, hand = 0;
};
SideCounts CountSides(const SoundData& d);
// A re-detection (or Reset / another mode) replaced `old` by `now`: each new frame that overlaps old ones takes the
// side (rule and hand) of the old frame it overlaps most, written (locked); a frame with no old one keeps 0 and
// continues the alternation from its neighbours (FrameRuleSides). Nothing is re-alternated. Runs only when `sides` (the
// Sound's Left / Right) or some old frame had a side written; Takes mode (`by_index`) matches frame k to frame k.
void CarrySides(std::vector<SoundFrame>& now, const std::vector<SoundFrame>& old, SidePattern p, bool sides_on, bool by_index = false);
// Divides the Sound by `group` with its value uids (in the group's order); group "" = combined. Cuts and
// syncs are dropped (RefreshParts measures them), and so are the "-> value" zones and role map entries
// whose value is not one of `values`. Excluded zones and hand syncs stay. False when nothing changed.
// 13-62 (decision 8): every part setting (`parts`) and every frame's owners are cleared too.
// 13-62i: to None, the zones stay where they are, all of the implicit part (kWholePart), gains and Gaps kept; to a
// group, they go (the variation keeps their extent and the first part zone's gain; RefreshParts proposes new ones).
bool SetDivide(SoundData& data, const std::string& group, const std::vector<std::string>& values);
// Sets the sync point mode and offset (ms). The measured syncs go, so a stored one is never kept under
// the old settings (RefreshParts measures them again; without peaks, placement does). Hand syncs stay.
// False when nothing changed or the offset is not finite.
bool SetSyncSettings(SoundData& data, SyncMode mode, double offset_ms);
// The part (value uid) an event of side-free role `role` gets from this Sound: its own role map, else
// its divide group's (SwitchValue::roles in `store`); "" (= the whole variation) when combined, for no
// role, a role no entry lists, or a value that is not one of its divide values.
std::string PartOfRole(const SoundStore& store, const SoundData& data, const std::string& role);

// ---- 13-62: per-part settings and comping (spec 13-62 Contract, decisions 1-14) ----------------------
// A divided Sound's part may own any of its settings (PartSettings, SoundData::parts); an unset one follows
// the Sound's. Each piece between two cuts has one owner (SoundFrame::owners; empty = the legacy k-th piece
// rule, PartAt); a part may own several separate pieces of a variation.

// 13-62h: an entry with nothing of its own (no setting, no lane fader, no unknown field); PruneEmptyParts removes
// every such entry.
bool PartSettingsEmpty(const PartSettings& p);
void PruneEmptyParts(SoundData& data);
// The part's settings entry, nullptr when none (or value "", or kWholePart: 13-62i, its settings are the Sound's).
const PartSettings* PartSettingsOf(const SoundData& data, const std::string& value);
// Effective values of a part: its own, else the Sound's (value "" = the Sound's). Gain: its own, else 0 dB
// (on top of the variation dB and the per-part Normalize).
double   PartGainDb(const SoundData& data, const std::string& value);
double   PartPrerollMs(const SoundData& data, const std::string& value);
double   PartTailMs(const SoundData& data, const std::string& value);
SyncMode PartSyncMode(const SoundData& data, const std::string& value);
double   PartSyncOffsetMs(const SoundData& data, const std::string& value);
MonoMode PartMono(const SoundData& data, const std::string& value);
// True when the part owns at least one fade (decision 6: then both edges of each of its pieces get its
// fades). `in` / `out` (s) get its own, else the Sound's default fade for that side.
bool PartFades(const SoundData& data, const std::string& value, double* in, double* out);

// Which setting of a part (ResetPartSetting).
enum class PartField { Preroll, Tail, SyncMode, SyncOffset, Gain, NormMode, NormTarget, Mono, FadeIn, FadeOut };
// Every field set in `patch` becomes the part's own (its `value` and `kept` ignored); non-finite numbers are
// ignored, margins / fades / norm mode kept >= 0 (norm mode < kNormModeCount). Nothing for a value that is
// not a divide value. 13-62i: kWholePart (an undivided Sound's one part) writes the Sound's own fields (pre-roll,
// tail, sync mode / offset, mono, fades; a gain or Normalize field has no Sound-level home and is ignored). A sync change drops the measured syncs, a pre-roll / tail change the part trims
// (RefreshParts measures them again, as SetSyncSettings).
void SetPartSetting(SoundData& data, const std::string& value, const PartSettings& patch);
// Back to the Sound's for one field / every field of the part (an entry left with nothing is removed). "All"
// keeps the lane fader (fader_db) and the unknown fields: they are not settings.
void ResetPartSetting(SoundData& data, const std::string& value, PartField field);
void ResetPartAll(SoundData& data, const std::string& value);
// How many settings the part owns (0 = "same as the Sound").
int OwnSettingCount(const SoundData& data, const std::string& value);
// 13-62 (E2, the part rows): two entries own the same settings (nullptr = none; a gesture that changed
// nothing writes nothing).
bool SamePartSettings(const PartSettings* a, const PartSettings* b);
// A setting the parts' sync points or margins are measured with (its change measures them again).
bool PartFieldRemeasures(PartField f);
// The part's own value of a number row in the row's unit (fades: ms); none while it follows the Sound
// (and for the non-number fields).
std::optional<double> OwnPartNumber(const PartSettings* p, PartField f);
// A patch setting number field `f` to `v` (the row's unit: fades in ms).
PartSettings PartNumberPatch(PartField f, double v);
// A number rounded to 0.1 as the mockup's sgn writes a gain: "+3", "-2.5", "0" (`sign` false: no "+").
std::string RoundedNumberText(double v, bool sign);

// The frame's owners give each of its pieces between cuts a divide value (one more than its cuts, each a
// divide value).
bool OwnersSane(const SoundData& data, const SoundFrame& f);

// ---- 13-20 / 13-23 / 13-25 part C: what the Edit window shows and its role gestures ------------------

// The excluded zones (`to` "") of the Sound.
int ExcludedZoneCount(const SoundData& data);
// True when the Sound's divide values are no longer its group's values (a value added, removed or
// reordered after dividing; the group deleted): the Divide row's amber note.
bool DivideStale(const SoundStore& store, const SoundData& data);
// The roles of each divide value (index k = divide_values[k]), from the Sound's own map, else its
// group's; `own` gets whether the Sound's own map is used. {} when combined.
std::vector<std::vector<std::string>> RolesByValue(const SoundStore& store, const SoundData& data, bool* own = nullptr);
// The mark kept in a Sound's own role map when the user took every role off it ("no role maps to a
// part for this sound", which an empty map cannot say: empty = the group's). Never a role key.
constexpr const char kNoRolesMark[] = "-";
// Toggles `role` on value `value_uid` in the Sound's own map (Roles... in Edit): the map is first
// seeded from the group's when the Sound has none; a role belongs to one value (it moves). False when
// the Sound is not divided or the value is not one of its divide values.
bool ToggleSoundRole(const SoundStore& store, SoundData& data, const std::string& value_uid, const std::string& role);
// Toggles `role` on a value of a Switch Group (Switch Groups > Roles): off when it has it, else on and
// off every other value of the group. `moved_from` gets the value uid it left ("" = none). False for an
// unknown group / value or an empty role.
bool ToggleGroupRole(SoundStore& store, const std::string& group_uid, const std::string& value_uid, const std::string& role,
                     std::string* moved_from = nullptr);

// ---- 13-28: Normalize (modes, scopes, per-part gains) -------------------------------------------------

// The gain (dB) a part of a frame plays at: its own (part_db[part_value], set by a "Per part" Normalize) or
// else the frame's. part_value "" (the whole variation) = the frame's.
double FrameGainDb(const SoundFrame& frame, const std::string& part_value);
// True when "Per part" is offered: a Sound divided by a Switch Group (13-62i: Takes mode too, each take has zones).
bool NormPartsAvailable(const SoundData& data);
// True when what a Normalize measures is the same in both: the frames (start, length, on, cuts), the zones,
// the divide group and values, the role map and the detect mode (the measuring job is cancelled otherwise).
bool SameNormMeasured(const SoundData& x, const SoundData& y);

// One stretch of audio Normalize measures and gives a gain: a variation (`part` "") or one part of it.
// `spans` = what is placed of it (excluded zones left out; a part cut by a zone has several spans), in
// file time; `path` = the file it is measured in (filled by the caller: VariationPath).
struct NormRegion {
    int                   frame = -1;
    std::string           part;
    std::vector<PartSpan> spans;
    std::string           path;
    // 13-62: what it is measured and normalized to (the caller's settings; a part's own: PartNormRegions).
    NormMode mode = NormMode::Peak;
    double   target = -1.0;
};
// 13-62 (C1): a part's Normalize mode and target: its own, else the Sound-level ones given (the user's
// LEVEL settings). value "" = the given ones.
NormMode PartNormMode(const SoundData& data, const std::string& value, NormMode sound_mode);
double   PartNormTarget(const SoundData& data, const std::string& value, double sound_target);
// REAPER's CalculateNormalization answer (a linear volume factor) in dB; NaN when it means silence (<= 0,
// not finite, or more than +60 dB).
double NormAdjustmentDb(double factor);
// One region's adjustment from its spans' adjustments (dB, NaN = silent span, left out): Peak, True peak,
// LUFS-M / LUFS-S max = the loudest span's (the smallest adjustment); RMS-I, LUFS-I = the energy over the
// summed duration (`durations`, s). NaN when every span is silent.
double CombineSpanAdjustments(NormMode mode, double target, const std::vector<double>& adj_db,
                              const std::vector<double>& durations);
// A region's adjustment in Peak mode from the file's cached peaks: target - its loudest span's peak; NaN when
// silent (no peaks there, or more than +60 dB).
double RegionPeakAdjDb(const PeakData& peaks, const NormRegion& region, double target);

// 13-46: the Sound panel's read-only line for a divided Sound: "Divided by <Group> (<value> / <value>)",
// its divide values in their order (a deleted value reads "?", a deleted group "?"); "" when combined.
std::string DividedByText(const SoundStore& store, const SoundData& data);

}  // namespace rav
