// SPDX-License-Identifier: MIT
//
// Sound entries (13-3): a Sound's variations (frames) from its takes, take-marker slices or silence.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-3
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#include <set>
#include <string>
#include <vector>

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
    std::vector<SoundFrame> takes;             // one per take of the item (start = its start offset)
    std::vector<Slice>      slices;            // the active take's slices (take markers or .rpp text)
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
// "-> value" zones and hand cuts go. The caller runs RefreshParts after (it has the peaks).
void ResetToDetected(SoundData& data, const std::vector<SoundFrame>& detected);
// The frame's peak (dBFS), -inf when the peaks do not cover it.
double FramePeakDb(const PeakData& peaks, const SoundFrame& frame);
// Sets each frame's db so its peak reaches target_db: every frame (whole) or the `selection` indices.
// Sets `hand` on each frame it changes; the caller (the Normalize gesture) sets `data.edited`.
void NormalizeFrames(std::vector<SoundFrame>& frames, const PeakData& peaks, double target_db, bool whole,
                     const std::vector<int>& selection);

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
bool SplitFrame(SoundData& data, int i, double at);
void SetFrameOn(SoundData& data, int i, bool on);
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
bool PartAt(const SoundData& data, int variation, double t, std::string* value);
// Every non-excluded contiguous stretch of the variation with its owner, in time order (a part cut by
// an excluded zone gives 2 spans; an empty part gives none). {} for a bad index or an off frame.
std::vector<PartSpan> PartSpansOf(const SoundData& data, int variation);
// What a placement of `part_value` plays: the spans of that part, or for "" every non-excluded stretch
// of the frame (adjacent spans merged: the whole variation minus its exclusions, value_uid "").
std::vector<PartSpan> PlacedSpans(const SoundData& data, int variation, const std::string& part_value);

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
double AutoSync(const SoundData& data, const PeakData* peaks, const std::vector<PartSpan>& spans);
// Keeps a time inside the spans (the nearest span edge when it falls between or outside them).
double ClampIntoSpans(const std::vector<PartSpan>& spans, double t);
// Refreshes one variation's cuts and sync points (every write that changes frames, detection, divide,
// sync settings or zones runs RefreshParts). Cuts: hand ones kept while there are parts - 1 of them,
// else AutoCuts (no peaks: the stored ones while their count fits, else an even split); none when
// combined. Syncs: per part, a hand sync inside its spans wins, else AutoSync; -1 for a part with no
// span. Without peaks, Attack / Peak syncs that cannot be kept (a stored one still inside its part)
// leave `syncs` empty: measured at placement (SyncsStale). Takes mode: no cuts, no syncs (A8).
void RefreshFrame(SoundData& data, int variation, const PeakData* peaks);
void RefreshParts(SoundData& data, const PeakData* peaks);
// True when the variation's syncs must be measured again: not one per part, or a part with spans whose
// sync is not inside them, or one without spans whose sync is set.
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
// (only when `with_cuts`). The marks `now` held are replaced.
void CarryHandMarks(std::vector<SoundFrame>& now, const std::vector<SoundFrame>& old, bool with_cuts = true);
// Divides the Sound by `group` with its value uids (in the group's order); group "" = combined. Cuts and
// syncs are dropped (RefreshParts measures them), and so are the "-> value" zones and role map entries
// whose value is not one of `values`. Excluded zones and hand syncs stay. False when nothing changed.
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
bool MoveCut(SoundData& data, int variation, int k, double t);
// Sets part `part`'s sync point by hand (clamped into its spans, 1 us inside an edge so a sync on a cut
// belongs to one part only; part 0 when combined): its other hand
// syncs go, `syncs[part]` follows. False for a bad index or an empty part.
bool MoveSync(SoundData& data, int variation, int part, double t);
// Back to RAV's sync points / cuts for that variation (the caller runs RefreshParts). False when
// nothing was moved by hand.
bool ResetSyncs(SoundData& data, int variation);
bool ResetCuts(SoundData& data, int variation);
// The part (value uid) an event of side-free role `role` gets from this Sound: its own role map, else
// its divide group's (SwitchValue::roles in `store`); "" (= the whole variation) when combined, for no
// role, a role no entry lists, or a value that is not one of its divide values.
std::string PartOfRole(const SoundStore& store, const SoundData& data, const std::string& role);

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

}  // namespace rav
