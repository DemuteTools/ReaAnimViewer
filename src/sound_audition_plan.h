// SPDX-License-Identifier: MIT
//
// Audition and pre-edit logic of the Library view's node / Sound panels (13-7b). Pure, no REAPER,
// host-tested (tests/sound_audition_plan_test.cpp). The REAPER side (sound_audition.cpp) plays
// the voices a plan lists; library_node_ui.cpp draws and wraps each edit in one undo point.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

// What an audition asks for each pick (so tests can force them).
struct AuditionPicker {
    // A Random's child: an index into ChildNodes(lib, node) of `count` children (-1 = none).
    std::function<int(const SoundNode& node, int count)> pick_child;
    // A Sound's variation: a frame index (-1 = none).
    std::function<int(const SoundNode& node)> pick_variation;
    // 13-23: a Sound's variation for a part (a value uid, "" = the whole variation; -1 = none). Used
    // instead of pick_variation when set: a variation with nothing of that part is never picked.
    std::function<int(const SoundNode& node, const std::string& part_value)> pick_part_variation;
};

// The session's picks per node, so each press draws anew while avoiding repeats (decision 5).
class AuditionHistory {
public:
    explicit AuditionHistory(uint64_t seed = 0x9E3779B97F4A7C15ULL) : seed_(seed) {}
    // A picker on DrawPick: Random children over every child, Sounds over ActiveVariations,
    // each avoiding the node's last `avoid_repeat` picks; every pick is remembered.
    AuditionPicker Picker();
    const std::vector<int>& History(const std::string& node) { return hist_[node]; }

private:
    uint64_t                                seed_;
    uint64_t                                press_ = 0;
    std::map<std::string, std::vector<int>> hist_;
};

struct AuditionVoice {
    std::string              node;           // the Sound
    int                      variation = -1;
    double                   start_s = 0.0;  // in its source
    double                   length_s = 0.0;
    double                   delay_ms = 0.0; // Blend offsets below the auditioned node
    double                   gain_db = 0.0;  // volumes from the library root + the frame's db
    double                   fade_in_s = 0.0;   // the variation's fades as placed (AuditionFades)
    double                   fade_out_s = 0.0;
    int                      fade_in_shape = 0;
    int                      fade_out_shape = 0;
    std::vector<std::string> path;           // uids from the auditioned node's child to the Sound
    int                      piece = 0;      // 13-20 / 13-23: its piece of the variation (0 = the first)
    // 13-29: a voice that is a window of a longer faded stretch (a part of a piece, or a voice restarted
    // mid-way): its fades are those of [fade_from_s, ...) inside a stretch of fade_span_s (0 = itself).
    double                   fade_from_s = 0.0;
    double                   fade_span_s = 0.0;
    MonoMode                 mono = MonoMode::Off;  // 13-41: the Sound's (13-62: its part's own, PartMono)
    std::string              part;           // 13-62: the part it plays (a value uid; "" = the whole / mixed)
};

// 13-62 (decision 5): the Edit window's Mute / Solo (view state, never saved): Solo adds (REAPER), and while
// any part is soloed only soloed parts are heard; else every part but the muted ones.
struct PartListen {
    std::set<std::string> mute, solo;
    bool                  Active() const { return !mute.empty() || !solo.empty(); }
};
// Whether part `value` is heard under `listen` ("" = no part: heard unless a part is soloed).
bool PartHeard(const PartListen& listen, const std::string& value);

struct AuditionPlan {
    std::string                root;  // the auditioned node
    std::vector<AuditionVoice> voices;
    std::string                played;  // "Layer 2 › grass_b + Sound A", "nothing" when no voice
    double                     length_ms = 0.0;  // max delay + length
};

// Antho 2026-10-12: the fades an audition of variation i plays, the same as Populate places
// (FadeApplyFor): the frame's own or the Sound's default fade; a Whole-item Sound only its own (the
// placed copy keeps its source item's fades, which an audition of the file does not read).
// A folder-pile source: none (its placed members keep their own fades).
void AuditionFades(const SoundData& data, int i, double* in_s, double* out_s, int* in_shape, int* out_shape,
                   bool folder_source = false);
// The gain of a voice's fades at `t` s into it (0..1): ramps up over `in` s from its start and down
// over `out` s to its `length` (in + out clamped to it; 1 between). Shapes = REAPER's index (0 linear,
// 1 / 3 fast start, 2 / 4 fast end, 5 S-curve; anything else linear). No allocation: the audio thread
// calls it per sample.
double FadeGainAt(double t, double length, double in, double out, int in_shape = 0, int out_shape = 0);

// What one press plays (decisions 5-6). A child with no active variation is skipped.
AuditionPlan PlanAudition(const SoundStore& store, const SoundLibrary& lib, const std::string& node_uid,
                          const AuditionPicker& picker);
// 13-20 / 13-23: the voices of variation `variation` of Sound `n` playing part `part_value` ("" = the
// whole variation minus its exclusions), as Populate places them (PieceApplyFor): one per piece, each
// delayed by its distance from the first piece plus `delay_ms`, the Sound's cut_fade at a cut or zone
// edge. Takes mode: the take whole (one voice). {} for a bad index or nothing left of that part.
// 13-62: the part's own gain (PartGainDb) on top, its mono (PartMono), its own fades per side (PieceApplyFor).
std::vector<AuditionVoice> PartAuditionVoices(const SoundLibrary& lib, const SoundNode& n, int variation,
                                              const std::string& part_value, double delay_ms = 0.0);
// 13-20: a zone of the Sound's source played alone (the Edit window's "Play zone"): one voice over
// [start, start + length), the Sound's cut_fade at both ends, the level of the frame holding its start
// (0 dB outside any frame). Empty voice (length 0) for a zone of 0 s or less.
AuditionVoice ZoneAuditionVoice(const SoundLibrary& lib, const SoundNode& n, double start, double length);
// The "played: ..." text of these voices (decision 6): "nothing" when none. 13-20: a voice of a
// later piece (piece > 0) is not named again.
std::string PlayedText(const SoundLibrary& lib, const std::vector<AuditionVoice>& voices);
// Sum of volume_db from the library root down to `n` (n included): a voice's level (decision 5).
double GainFromRoot(const SoundLibrary& lib, const SoundNode& n);
// The nodes lit at t ms into the plan: the root and the path of each voice sounding then.
std::set<std::string> LitAt(const AuditionPlan& plan, double t_ms);

// The child a Switch plays in the panel: preview_value while it is a value of the Switch's group,
// else DefaultValue ("" = none; a value with no content plays nothing).
std::string SwitchPreviewChild(const SoundStore& store, const SoundLibrary& lib, const SoundNode& sw);

struct LengthRange {
    double min_s = 0.0;
    double max_s = 0.0;
    bool   varies() const { return max_s - min_s > 1e-6; }
};
// Sound: its active frames; Random: min / max of its children that have something to play; Blend: offset + length (max over
// parts); Switch: the preview child. An empty container: {0, 0}.
LengthRange NodeLengthRange(const SoundStore& store, const SoundLibrary& lib, const std::string& uid);

// Decision 7: a re-detection keeps the hand frames; detected frames overlapping one are dropped. 13-62g review: a
// hand frame not `trimmed` (its edges never moved by hand: a gain, fade, on / off or Normalize edit) takes the range
// of the detected frame it overlaps most, its edits kept (none overlapping, or the range overlapping another hand
// frame: its own range).
// `by_index` (Takes mode, frame i = take i): a hand frame replaces the detected frame at its index.
std::vector<SoundFrame> MergeHandFrames(const std::vector<SoundFrame>& detected,
                                        const std::vector<SoundFrame>& previous, bool by_index = false);

// Decision 3: Takes mode trims frame i inside [0, take_source_len] only. False when unchanged
// (bad index, or 1 ms or less left). Sets `hand` and `data.edited`.
bool TrimTakeFrame(SoundData& data, int i, double start, double length, double take_source_len);

// Whether a playing audition has ended (the play button then shows Play again): its position
// reached the mix's length (within an audio block), or it stopped moving for `stalled_s` seconds
// (a preview that stops short of the end). Polled each frame.
bool AuditionEnded(double pos, double length, double stalled_s);

enum class FrameEdit { Trim, Add, Split, Toggle, Db };
// Decision 3: Takes mode refuses Add and Split.
bool FrameEditAllowed(DetectMode mode, FrameEdit edit);

// ---- 13-61: REAPER's item trims in the Sound Edit window ----------------------------------------------
// "Trim left of cursor" (REAPER 40511): a variation that holds the edit cursor starts there; "right of
// cursor" (40512): it ends there; "to selected zone" (40508): each variation the zone crosses keeps only
// what is inside the zone. The variations: the selected ones (Edit's selection), else -- nothing selected
// -- the one under the cursor (the cursor trims) or every one the zone crosses. Trims only shrink a
// variation (never past its neighbours, never 1 ms or less: that one is left as it is). Not in Takes
// mode (each take has its own time: the caller refuses it).
enum class EditTrimKind { LeftOfCursor, RightOfCursor, ToZone };

struct EditTrimStep {
    int    frame = -1;
    double start = 0.0, length = 0.0;  // its new stretch (source seconds)
};

// 13-62F (F4): a stretch of a variation excluded (ExcludeRange) by a piece-bounded trim.
struct EditTrimExclude {
    int    frame = -1;
    double a = 0.0, b = 0.0;  // source seconds, a < b
};

struct EditTrimPlan {
    std::vector<EditTrimStep>    steps;     // in frame order; empty with `excludes` = nothing to do (`why` says what is missing)
    std::vector<EditTrimExclude> excludes;  // 13-62F (F4), in frame order
    std::string                  why;
    bool Empty() const { return steps.empty() && excludes.empty(); }
};

// `cursor` < 0 = no edit cursor; `zone_len` <= 0 = no zone.
EditTrimPlan PlanEditTrim(const std::vector<SoundFrame>& frames, const std::vector<int>& selected, EditTrimKind kind,
                          double cursor, double zone_start, double zone_len);
// 13-62F (F4, Antho 2026-10-14): on a divided Sound the cursor trims act on the piece under the edit cursor
// only (EditPiecesOf: between the frame's edges, its cuts and the zones' edges; adjacent placed pieces of the
// same part read as one, so it stops only at a visible part change or an excluded stretch). Trim left: when that piece
// starts at the variation's start, the variation starts at the cursor (as above); else [piece start, cursor]
// is excluded (hatched on every lane, the other pieces untouched). Trim right: the same at the piece's end.
// A cursor on a piece boundary, an excluded stretch away from the variation's edge, or 1 ms or less:
// nothing. Trim to zone, an off variation and an undivided Sound: the frames' plan above.
EditTrimPlan PlanEditTrim(const SoundData& data, const std::vector<int>& selected, EditTrimKind kind, double cursor,
                          double zone_start, double zone_len);

// 13-62F (F5, Antho 2026-10-14): REAPER's "Item navigation: Select and move to next / previous item" (40417 /
// 40416) in the Sound Edit window, to chain through the variations at a fixed zoom. From the last selected
// variation (`selected`'s last valid one): the next / previous one in time order (off ones included, as REAPER
// walks every item; ties in start by index; `by_index` (Takes mode, each take has its own time): index order).
// Nothing selected, as REAPER: next = the first one starting after the edit cursor, previous = the last one
// starting before it (the one under the cursor counts for previous only when the cursor is past its start);
// no cursor (`cursor` < 0) or `by_index`: the first / the last one. It is selected, the edit cursor goes to its
// start and the view (`view_len` s, never changed; <= 0 = the whole source) is centred on it, clamped to
// [0, source_len]. At the last / first one (or no variation at all): `frame` -1, nothing moves.
struct EditNavPlan {
    int    frame = -1;
    double cursor = 0.0;
    double view_start = 0.0;
};
EditNavPlan PlanEditNav(const std::vector<SoundFrame>& frames, const std::vector<int>& selected, double cursor, bool next,
                        double view_len, double source_len, bool by_index = false);

// 13-63: REAPER's "Split items at edit cursor" (40757) in the Sound Edit window: each variation that holds the
// edit cursor -- among the selected ones, else (nothing selected) among them all -- split there, as the
// waveform's S used to split the one under the mouse. A piece of 1 ms or less would be left: that one stays
// whole. Not in Takes mode (the caller refuses it).
struct EditSplitPlan {
    std::vector<int> frames;  // ascending; empty = nothing to do (`why` says what is missing)
    std::string      why;
};

// `cursor` < 0 = no edit cursor.
EditSplitPlan PlanEditSplit(const std::vector<SoundFrame>& frames, const std::vector<int>& selected, double cursor);

// The selection Normalize works on: the selected frames (NormalizeRegions keeps the on ones); {} =
// every frame (no selection); {-1} = none when only off frames are selected.
std::vector<int> NormalizeSelection(const std::vector<SoundFrame>& frames, const std::vector<int>& selected);
// 13-29 (Antho 2026-10-13: Play in Edit plays exactly what is shown): the Edit window's Play, on the file's
// clock (a voice's delay = its start in the file, so the playhead follows the file). Every variation that is
// on plays as Populate places it on a combined event (PartAuditionVoices with part ""): its trims, its fades,
// its dB, the excluded zones silent with the Sound's cut_fade at their edges; off variations and the file
// between variations are silent. A divided variation whose parts have their own gain (13-28 part_db) plays
// each part at it, one voice per part, the piece's fades kept across them (fade_from_s / fade_span_s).
// 13-54: a divided Silence-detected Sound (PartMarginsApply) plays only what its parts play once their
// pre-roll / tail apply (PartPieces): what the margins leave out is silent, with the Sound's cut fade at
// the sides they cut; touching parts play on as one voice (split only for part gains).
// Overlapping variations: the earlier one wins until it ends. Takes mode: take `take` whole as placed
// (each take is its own file), at delay 0; -1 or a bad index = nothing.
// 13-62: a divided variation with part settings or a `listen` filter is played per owner too: each stretch
// at FrameGainDb + PartGainDb of its part, in its part's mono; a part owning a fade plays its stretch as
// its own voice, its owned side(s) faded with the part's fade (decision 6, per side; an unowned side keeps
// the piece's fade at the piece's edge, nothing inside it next to a heard part); a part not heard (PartHeard) is
// silent, the Sound's cut_fade on the sides of what plays next to it. No part settings and no filter = as
// before.
std::vector<AuditionVoice> EditPlayVoices(const SoundLibrary& lib, const SoundNode& n, int take = -1,
                                          const PartListen* listen = nullptr);
// 13-62 (decision 13, Space): piece `piece_index` of variation `variation` (EditPiecesOf's index: unmerged,
// an excluded piece too) alone, delay 0: its part's gain and mono; per side its part's own fade, else
// the variation's fade at a variation edge and the Sound's cut_fade at a cut / zone edge. Never filtered by
// Mute / Solo (an explicit pick). {} for a bad index, Takes mode or a piece of 0 s.
std::vector<AuditionVoice> PiecePlayVoices(const SoundLibrary& lib, const SoundNode& n, int variation, int piece_index);
// 13-62 (decision 13, Shift+Space): every piece of part `value` in file order: each on variation's
// PartAuditionVoices (as placed), delayed by its distance in the file from the first one (the playhead walks
// the file; the other parts' stretches are silent). Overlapping variations: the earlier one wins until it ends
// (as EditPlayVoices), never summed. {} for value "", Takes mode or no piece of that part.
std::vector<AuditionVoice> PartPiecesVoices(const SoundLibrary& lib, const SoundNode& n, const std::string& value);
// The voices as heard from `from_s` s into them (a Play restarted where it was): those over are dropped,
// the one under it cut (start, length, fade_from_s moved), every delay shifted back by from_s.
std::vector<AuditionVoice> VoicesFrom(const std::vector<AuditionVoice>& voices, double from_s);
// Whether two voice lists play the same (a changed one restarts Edit's Play).
bool SameVoices(const std::vector<AuditionVoice>& a, const std::vector<AuditionVoice>& b);
// 13-49 review: when Edit's Play restarts on a change heard live. `playing` = the voices it plays, `now` =
// the Sound as shown. The voices last seen different from `playing` are kept with the time they appeared;
// any new change resets that time, so Play restarts (true) only once `now` stayed the same `settle_s`
// (a long drag restarts when it pauses or ends, never every `settle_s` during it). Back to `playing`
// (an undone change) = nothing pending.
struct EditPlaySettle {
    std::vector<AuditionVoice> seen;
    double                     since = -1.0;  // < 0 = no change pending
};
bool SettleEditPlay(EditPlaySettle& s, const std::vector<AuditionVoice>& now, const std::vector<AuditionVoice>& playing,
                    double t, double settle_s = 0.15);
// 13-41: a voice's sample forced to mono from its left / right samples: Left l, Right r, Sum (l + r) / 2;
// Off l (the caller keeps the channels). The peaks drawn use it too (per column, on the lanes' max abs).
double MonoOf(double l, double r, MonoMode mono);

// 13-40: what the Play button / Space plays when child `child_uid` of `parent_uid` is selected in its Mix
// section: the child's own resolution (a Random draws, a Switch its preview / default value, a Blend its
// layers), named from the child ("Child › grass_b"), with the parent as the plan's root (the parent's panel
// shows Stop and "played: ..."; the child's row is lit). An unknown child: an empty plan.
AuditionPlan PlanChildAudition(const SoundStore& store, const SoundLibrary& lib, const std::string& parent_uid,
                               const std::string& child_uid, const AuditionPicker& picker);

// "D → N variations · K off" (decision 7): "D → " dropped when D = N, " · K off" when K = 0;
// "1 variation" singular.
std::string VariationCountText(int detected, const std::vector<SoundFrame>& frames);

}  // namespace rav
