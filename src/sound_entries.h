// SPDX-License-Identifier: MIT
//
// Sound entries (13-3): a Sound's variations (frames) from its takes, take-marker slices or silence.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-3
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

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
// The frame's peak (dBFS), -inf when the peaks do not cover it.
double FramePeakDb(const PeakData& peaks, const SoundFrame& frame);
// The peak (dBFS) over [start, start + length) source seconds, -inf when the peaks do not cover it.
double SpanPeakDb(const PeakData& peaks, double start, double length);

// The default mode of a new entry (DECISIONS Q24): Takes when >= 2 takes, else TakeMarkers when
// >= 2 slices, else Silence when peaks exist, else Whole.
DetectMode DefaultDetectMode(const EntryFacts& facts, bool has_peaks);

// Hand edits (pre-edit). Each sets `hand` on the touched frame and `data.edited`; frames never
// overlap (a frame is clamped between its neighbours' edges).
// Moves / resizes frame i inside [previous end, next start] and [0, source_len]; unchanged when
// that leaves 1 ms or less.
void TrimFrame(SoundData& data, int i, double start, double length, double source_len);
// Adds a frame clamped into the gap that holds `start`; its index, -1 when `start` is inside a
// frame or there is no room.
int AddFrame(SoundData& data, double start, double length, double source_len);
// Splits frame i at source time `at` into 2 frames (on and db kept); false when `at` is outside it.
// 13-62: a frame with owners gives each half the cuts and owners on its side (the piece holding `at` is
// in both); without owners both halves keep every cut as before (RefreshParts drops the ones outside).
bool SplitFrame(SoundData& data, int i, double at);
void SetFrameOn(SoundData& data, int i, bool on);
// 13-28: a frame with per-part gains moves them by the same step (their balance kept).
void SetFrameDb(SoundData& data, int i, double db);

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
// Sets frame i's own fades (s; < 0 = back to the Sound's default for that side), clamped to its
// length: the dragged side gives way to the other one. Marks `hand` and `data.edited`.
void SetFrameFades(SoundData& data, int i, double in, double out);
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
// The index of a value in divide_values, -1 when absent.
int PartIndex(const SoundData& data, const std::string& value_uid);
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
double AutoSync(const SoundData& data, const PeakData* peaks, const std::vector<PartSpan>& spans);
// Keeps a time inside the spans (the nearest span edge when it falls between or outside them).
double ClampIntoSpans(const std::vector<PartSpan>& spans, double t);
// Refreshes one variation's cuts and sync points (every write that changes frames, detection, divide,
// sync settings or zones runs RefreshParts). Cuts: hand ones kept while there are parts - 1 of them,
// else AutoCuts (no peaks: the stored ones while their count fits, else an even split); none when
// combined. 13-62: a variation with sane owners (one per piece, each a divide value) keeps its cuts,
// whatever their count; owners that do not fit are dropped. Syncs: per part, a hand sync inside its spans wins, else AutoSync; -1 for a part with no
// span. Without peaks, Attack / Peak syncs that cannot be kept (a stored one still inside its part)
// leave `syncs` empty: measured at placement (SyncsStale). Takes mode: no cuts, no syncs (A8).
void RefreshFrame(SoundData& data, int variation, const PeakData* peaks);
void RefreshParts(SoundData& data, const PeakData* peaks);
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
// with them); a frame whose owners hold no cut follows its middle.
void CarryHandMarks(std::vector<SoundFrame>& now, const std::vector<SoundFrame>& old, bool with_cuts = true);
// Divides the Sound by `group` with its value uids (in the group's order); group "" = combined. Cuts and
// syncs are dropped (RefreshParts measures them), and so are the "-> value" zones and role map entries
// whose value is not one of `values`. Excluded zones and hand syncs stay. False when nothing changed.
// 13-62 (decision 8): every part setting (`parts`) and every frame's owners are cleared too.
bool SetDivide(SoundData& data, const std::string& group, const std::vector<std::string>& values);
// Sets the sync point mode and offset (ms). The measured syncs go, so a stored one is never kept under
// the old settings (RefreshParts measures them again; without peaks, placement does). Hand syncs stay.
// False when nothing changed or the offset is not finite.
bool SetSyncSettings(SoundData& data, SyncMode mode, double offset_ms);
// Adds a zone (to "" = exclude); a zone it covers fully is dropped. False when 1 ms or less.
bool AddZone(SoundData& data, double start, double length, const std::string& to);
// Removes every excluded zone over source time `t` ("Include again"). False when none.
bool IncludeAgain(SoundData& data, double t);
// Moves cut `k` of variation `variation` by hand (kept between its neighbours and inside the frame, 1
// ms apart): `cuts_hand` set, `cut_unsure` cleared. False when nothing moved.
// 13-62 (C4): on a variation with owners, a cut dragged so a piece next to it would be under 4 ms removes
// that piece and the cut (the piece on the other side takes its place); a legacy variation (no owners)
// keeps the 1 ms clamp.
bool MoveCut(SoundData& data, int variation, int k, double t);
// Sets part `part`'s sync point by hand (clamped into its spans, 1 us inside an edge so a sync on a cut
// belongs to one part only; part 0 when combined): its other hand
// syncs go, `syncs[part]` follows. False for a bad index or an empty part. 13-54 review: a hand sync
// belongs to the part owning its spot by the cuts and zones (PartAt, the raw owner), whatever the
// margins: it is kept inside both that part's raw spans and its pieces, only the hand syncs that part
// owns are replaced, and a refresh clamps each hand sync into its owner's pieces (a margin that trims
// its spot away moves it to the nearest edge of its own part, never to a neighbour).
bool MoveSync(SoundData& data, int variation, int part, double t);
// Back to RAV's sync points / cuts for that variation (the caller runs RefreshParts). False when
// nothing was moved by hand.
// 13-62: ResetCuts also clears the variation's owners (back to the legacy rule).
bool ResetSyncs(SoundData& data, int variation);
bool ResetCuts(SoundData& data, int variation);
// The part (value uid) an event of side-free role `role` gets from this Sound: its own role map, else
// its divide group's (SwitchValue::roles in `store`); "" (= the whole variation) when combined, for no
// role, a role no entry lists, or a value that is not one of its divide values.
std::string PartOfRole(const SoundStore& store, const SoundData& data, const std::string& role);

// ---- 13-62: per-part settings and comping (spec 13-62 Contract, decisions 1-14) ----------------------
// A divided Sound's part may own any of its settings (PartSettings, SoundData::parts); an unset one follows
// the Sound's. Each piece between two cuts has one owner (SoundFrame::owners; empty = the legacy k-th piece
// rule, PartAt); a part may own several separate pieces of a variation.

// The part's settings entry, nullptr when none (or value "").
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
// not a divide value. A sync change drops the measured syncs, a pre-roll / tail change the part trims
// (RefreshParts measures them again, as SetSyncSettings).
void SetPartSetting(SoundData& data, const std::string& value, const PartSettings& patch);
// Back to the Sound's for one field / every field of the part (an entry left with nothing is removed).
void ResetPartSetting(SoundData& data, const std::string& value, PartField field);
void ResetPartAll(SoundData& data, const std::string& value);
// How many settings the part owns (0 = "same as the Sound").
int OwnSettingCount(const SoundData& data, const std::string& value);

// One piece of a variation as the Edit window shows it (decision 10: never merged): between two
// boundaries (the frame's edges, its cuts, the zones' edges). `owner` = PartAt's ("" when combined or
// excluded); `index` = its place in EditPiecesOf's list.
struct EditPiece {
    double      start = 0.0;
    double      length = 0.0;
    std::string owner;
    bool        excluded = false;
    int         index = 0;
};
// The unmerged pieces of a variation in time order (excluded ones included, flagged); {} for a bad index,
// an empty or an off frame.
std::vector<EditPiece> EditPiecesOf(const SoundData& data, int variation);
// Comping (decision 9): gives [a, b] of the variation to part `value`: cuts at a and b when inside the
// frame and not already there (an edge within 4 ms of a cut or the frame's edge lands on it), the owners of
// the pieces it covers = value, any zone coverage inside [a, b] removed (an excluded stretch there is
// included again). The variation's owners are filled first (from the legacy rule) when empty; the cuts
// become hand cuts. False (nothing changed) when [a, b] inside the frame is under 4 ms, the value is not a
// divide value, the frame is off, Takes mode, or nothing changed.
bool AssignRange(SoundData& data, int variation, double a, double b, const std::string& value);
// The nearest cut or frame edge of the variation within `tolerance_s` of `t`, else `t` (the swipe's
// 6 px snap).
double SnapToCut(const SoundData& data, int variation, double t, double tolerance_s);
// Excludes [a, b] of the variation (AddZone with "", clipped to the frame). False when that leaves 1 ms or
// less or the index is bad.
bool ExcludeRange(SoundData& data, int variation, double a, double b);

// ---- 13-20 / 13-23 / 13-25 part C: what the Edit window shows and its role gestures ------------------

// The excluded zones (`to` "") of the Sound.
int ExcludedZoneCount(const SoundData& data);
// The excluded stretches of the variations (the part list's "Excluded" count): an off variation counts
// one, an on one each stretch of it its spans leave out.
int ExcludedStretchCount(const SoundData& data);
// The variations holding at least one span of `value_uid` ("" = any span: not fully excluded).
int VariationsWithPart(const SoundData& data, const std::string& value_uid);
// The sync points moved by hand (every hand sync), the cuts moved by hand (of the on variations; 0 when
// combined), and the on variations with an unsure cut RAV proposed.
int HandSyncCount(const SoundData& data);
int HandCutCount(const SoundData& data);
int UnsureCutCount(const SoundData& data);
// Whether part `part`'s sync point of the variation was moved by hand (a hand sync inside its spans).
bool SyncIsHand(const SoundData& data, int variation, int part);
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
// The gain shown on a variation's box: divided, the largest of its current parts' own gains (when any),
// else the frame's dB.
double FrameLabelDb(const SoundData& data, int variation);
// True when a stretch of variation `variation` plays past 0 dBFS: a span's peak (cached peaks of its file)
// plus the gain it plays at (its part's, else the frame's; 13-62: plus the part's own gain). An off frame never.
bool FrameClips(const SoundData& data, int variation, const PeakData& peaks);
// True when "Per part" is offered: a Sound divided by a Switch Group, not in Takes mode (a take plays whole).
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
};
// The regions of a Normalize: each on frame of `selection` (NormalizeSelection's: {} = every frame, {-1} =
// none) once (Per variation, Whole sound), or once per part with something left (Per part; on an undivided
// Sound, as Per variation). A frame with nothing left gives none.
std::vector<NormRegion> NormalizeRegions(const SoundData& data, NormScope scope, const std::vector<int>& selection);
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

struct NormalizeOutcome {
    int  done = 0;     // regions given a gain (Whole: the frames)
    int  skipped = 0;  // silent regions left as they are
    bool changed = false;
};
// Writes the gains (adj_db[i] for regions[i]; NaN or > +60 = silent, skipped). Per variation: each region's
// frame.db; Per part: frame.part_db[part], and frame.db = the smallest of its part dBs (what the whole
// variation plays at); Whole: every measured frame at the smallest
// adjustment (the loudest reaches the target). Per variation and Whole clear the acted frames' part_db. Each
// written frame gets `hand`; the caller sets `data.edited`.
NormalizeOutcome ApplyNormalize(std::vector<SoundFrame>& frames, NormScope scope, const std::vector<NormRegion>& regions,
                                const std::vector<double>& adj_db);

// 13-46: the Sound panel's read-only line for a divided Sound: "Divided by <Group> (<value> / <value>)",
// its divide values in their order (a deleted value reads "?", a deleted group "?"); "" when combined.
std::string DividedByText(const SoundStore& store, const SoundData& data);

}  // namespace rav
