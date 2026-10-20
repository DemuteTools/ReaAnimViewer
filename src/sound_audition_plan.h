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
#include "sound_zones.h"  // 13-62h: ZoneRef, HeardPieces

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
    // 13-68 v3 (audition = placement): a Blend's lead Sound's full rounds of its variations drawn before its pick (null = 0),
    // and a linked layer's pick made `linked` (recorded in its history; null = nothing recorded): as ResolveHooks.
    std::function<int(const SoundNode& node, const std::string& part_value)>             rounds;
    std::function<void(const SoundNode& node, const std::string& part_value, int linked)> link;
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
    std::map<std::string, int>              drawn_;  // 13-68 v3: picks per key, uncapped (a Blend's rounds)
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
    double                   fade_in_dir = NAN;   // 13-75: their curvatures (NaN = the shape's preset)
    double                   fade_out_dir = NAN;
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

// The gain of a voice's fades at `t` s into it (0..1): ramps up over `in` s from its start and down
// over `out` s to its `length` (in + out clamped to it; 1 between). Shapes = REAPER's index, curvatures
// REAPER's D_FADEINDIR / D_FADEOUTDIR (13-75: fade_curve.h, REAPER's curves exactly; NaN = the shape's preset,
// as REAPER sets it with the shape). No allocation: the audio thread calls it per sample.
double FadeGainAt(double t, double length, double in, double out, int in_shape = 0, int out_shape = 0, double in_dir = NAN,
                  double out_dir = NAN);

// What one press plays (decisions 5-6). A child with no active variation is skipped.
AuditionPlan PlanAudition(const SoundStore& store, const SoundLibrary& lib, const std::string& node_uid,
                          const AuditionPicker& picker);
// 13-20 / 13-23: the voices of variation `variation` of Sound `n` playing part `part_value` ("" = the
// whole variation minus its exclusions), as Populate places them (PieceApplyFor): one per piece, each
// delayed by its distance from the first piece plus `delay_ms`, the Sound's cut_fade at a cut or zone
// edge. Takes mode: the take whole (one voice). {} for a bad index or nothing left of that part.
// 13-62: the part's own gain (PartGainDb) on top, its mono (PartMono), its own fades per side (PieceApplyFor).
// 13-62h review (B2): a Sound placed from its zones (PlacedFromZones) plays exactly what Populate places: each
// piece delayed by its PieceApply::from_anchor (heard time: a Gap jumped, its two sides crossfading), at the
// root's volumes + its zone's gain (PieceApply::db); FrameGainDb / PartGainDb are not added.
std::vector<AuditionVoice> PartAuditionVoices(const SoundLibrary& lib, const SoundNode& n, int variation,
                                              const std::string& part_value, double delay_ms = 0.0);
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

// Decision 7: a re-detection keeps the hand frames; detected frames overlapping one are dropped.
// `by_index` (Takes mode, frame i = take i): a hand frame replaces the detected frame at its index.
std::vector<SoundFrame> MergeHandFrames(const std::vector<SoundFrame>& detected,
                                        const std::vector<SoundFrame>& previous, bool by_index = false);

// Whether a playing audition has ended (the play button then shows Play again): its position
// reached the mix's length (within an audio block), or it stopped moving for `stalled_s` seconds
// (a preview that stops short of the end). Polled each frame.
bool AuditionEnded(double pos, double length, double stalled_s);

// Fix 2026-10-19 (Loop not regular): a self-looping audition -- its head (`head_s` long: from the cursor to the loop's
// end) played once, then its loop pass (`loop_s` long) repeated by the mixing source itself, on the audio thread, so
// the loop point is sample-accurate whatever the UI's frame rate. LoopSegAt: the stretch of a block that starts at mix
// time `t` with `left` samples to fill at `sr`: whether it reads the loop pass, the time it reads from (in the head, or
// in the pass), and how many samples until the next boundary (>= 1, <= left). loop_s <= 0 = no loop (all head).
struct LoopSeg {
    bool   body = false;
    double t = 0.0;
    int    n = 0;
};
LoopSeg LoopSegAt(double t, int left, double sr, double head_s, double loop_s);
// The position shown for a raw mix time: in the head as is, then inside the current pass (0 .. loop_s); which pass
// (0 = the head, 1 = the first repeat, ...).
double LoopedPosition(double raw, double head_s, double loop_s);
int    LoopedPass(double raw, double head_s, double loop_s);

// 13-41: a voice's sample forced to mono from its left / right samples: Left l, Right r, Sum (l + r) / 2;
// Off l (the caller keeps the channels). The peaks drawn use it too (per column, on the lanes' max abs).
double MonoOf(double l, double r, MonoMode mono);

// 13-40: what the Play button / Space plays when child `child_uid` of `parent_uid` is selected in its Mix
// section: the child's own resolution (a Random draws, a Switch its preview / default value, a Blend its
// layers), named from the child ("Child › grass_b"), with the parent as the plan's root (the parent's panel
// shows Stop and "played: ..."; the child's row is lit). An unknown child: an empty plan.
AuditionPlan PlanChildAudition(const SoundStore& store, const SoundLibrary& lib, const std::string& parent_uid,
                               const std::string& child_uid, const AuditionPicker& picker);

// ---- 13-62h (Lot C): a zoned Sound's Edit playback (UsesZones) ---------------------------------------------
// What is heard over [from, to] (HeardPieces): each part zone's piece one voice (its file stretch, its zone's
// gain, its envelope = ZoneEnv: its own / cut / part / Sound fades, a Gap's crossfade on a side a Gap holds), laid
// in heard time: a Gap is jumped (the two sides overlap by xl + xr: two voices crossfading), empty space is silence
// (time passes, no voice). A part not heard under the Mute / Solo state (PartHeard), and a zone of an off
// variation, give no voice but keep their time. Heard time 0 = `from`. 13-62h review (R3): a heard zone whose
// Gap leads to a silent one (muted / not soloed) is cut at that Gap's edge with the At cuts fade (ZoneCutFade):
// it never plays the jumped audio against nothing; its span and voice start / end there, the heard time kept.
// (As Populate cuts a zone whose neighbour across a Gap is not placed in the same event.)

// One stretch of heard time: [at, at + (b - a)) plays the file's [a, b].
struct HeardSpan {
    double  at = 0.0;           // heard start (s)
    double  a = 0.0, b = 0.0;   // its file stretch (s)
    ZoneRef z;                  // {-1, -1} = empty space (silence)
    bool    heard = true;       // false: muted / not soloed / off variation (no voice)
    // Its envelope (as ZoneEnv): it rises over `fin` s from `lo` and falls over `fout` s to `hi` (13-75: on curves cin /
    // cout, as its voice).
    double  lo = 0.0, hi = 0.0, fin = 0.0, fout = 0.0;
    FadeCurve cin, cout;
};
// Heard time -> file time.
struct HeardMap {
    std::vector<HeardSpan> spans;  // in heard order (a crossfade = two spans overlapping in heard time)
    double                 total = 0.0;  // the heard length (s)
};
struct EditPlay {
    std::vector<AuditionVoice> voices;  // delay = heard start; empty when nothing is heard
    HeardMap                   map;
};
// {} for to <= from. The library overload adds GainFromRoot and the node's uid. 13-62i: on timeline `tl` (a Takes-mode
// Sound's take; sound_zones.h ZoneTimeline).
EditPlay EditPlayFrom(const SoundData& d, double from, double to, const PartListen* listen = nullptr, int tl = -1);
EditPlay EditPlayFrom(const SoundLibrary& lib, const SoundNode& n, double from, double to,
                      const PartListen* listen = nullptr, int tl = -1);

// A playhead at heard time t: its file time, its gain (the envelope there; 1 in silence), whether it is the
// incoming one of a crossfade (the later span: drawn dashed, rising; the outgoing one solid, fading). Up to two
// in a crossfade, one elsewhere (in empty space too: it walks the silence in file time), none at / past the end.
struct HeardHead {
    double  file_t = 0.0;
    double  gain = 1.0;
    bool    incoming = false;
    bool    heard = true;  // the span's (a muted part's playhead still moves)
    ZoneRef z;             // {-1, -1} in silence
};
std::vector<HeardHead> HeardHeads(const HeardMap& map, double heard_t);

// The play's range: from / to in the file. `ok` false = nothing to play (bad zone, empty selection).
struct EditRange {
    double from = 0.0, to = 0.0;
    bool   ok = false;
};
// The end of the chain of part zones Gaps glue after `z` (mockup chainEnd; a Gap: its left neighbour's chain);
// z's end when alone.
double ChainEnd(const SoundData& d, const ZoneRef& z);
// A zone's Play: a part zone from its start to its chain's end; a Gap's "Hear the jump": from
// max(its left zone's start, its start - 0.12 s) to min(its right zone's end, its end + 0.12 s).
EditRange ZonePlayRange(const SoundData& d, const ZoneRef& z);
// The selection's Play: one zone = ZonePlayRange; several = from the first one's start to the furthest chain end.
EditRange SelectionPlayRange(const SoundData& d, const std::vector<ZoneRef>& sel);
// Click plays: a part zone from the click `t` to its chain's end; a Gap from its start - 0.12 s to its end +
// 0.12 s (inside [0, file_len]; file_len <= 0 = no upper clamp).
EditRange ClickPlayRange(const SoundData& d, const ZoneRef& z, double t, double file_len);

// The transport rule (Antho 2026-10-14): every play starts at the edit cursor. A play asked over [from, to]: the
// cursor goes to `from` (inside [0, file_len]); with a time selection the playhead never leaves it: from the
// cursor when inside [a, b), else from a (the cursor goes there), up to b, and with Loop it repeats [a, b]; no
// time selection + Loop: a play to the file's end repeats the whole file. REAPER's preview has no loop (C4):
// the UI restarts EditPlayFrom(loop_a, loop_b) when the play reaches its heard end (EditLoopDue).
struct EditTransport {
    double from = 0.0, to = 0.0;  // what EditPlayFrom plays first
    double cursor = 0.0;          // the edit cursor after the press
    bool   loops = false;
    double loop_a = 0.0, loop_b = 0.0;  // repeated after the first pass (loops only)
};
EditTransport PlanEditPlay(double from, double to, bool has_tsel, double tsel_a, double tsel_b, bool loop,
                           double file_len);
// Space / the header's Play: from the edit cursor (< 0 = none: the file's start) to the file's end.
EditTransport PlanEditTransport(double cursor, bool has_tsel, double tsel_a, double tsel_b, bool loop, double file_len);
// Whether a looping play at heard position `pos` of a pass `total` s long restarts now (within `block_s`, an
// audio block, of its end).
bool EditLoopDue(const EditTransport& tp, double pos, double total, double block_s = 0.0);
// 13-67a (Loop sometimes stops): a looping play that ended between two frames (the preview reached its end, so it is
// gone before EditLoopDue could see it within a frame of its end) restarts too: true when it loops and its heard
// position last seen (`last_pos`) plus the wall time since then (`since_s`) reaches the pass's end (`total`, 50 ms of
// slack). A play stopped earlier (Esc, Stop) falls short of it. Review: only when last seen at most 0.5 s ago -- an
// Edit window not drawn for longer (closed, hidden) never restarts a play that ended meanwhile.
bool EditLoopEndedAtEnd(const EditTransport& tp, double last_pos, double since_s, double total);

// "D → N variations · K off" (decision 7): "D → " dropped when D = N, " · K off" when K = 0;
// "1 variation" singular.
// 13-67b: why Threshold / Min gap leave variations alone (Silence mode): those edited by hand keep their zones over a
// re-detection (MergeHandFrames). "" when none is.
std::string HandFramesText(const std::vector<SoundFrame>& frames);
std::string VariationCountText(int detected, const std::vector<SoundFrame>& frames);

// Freeze fix (audit #1): what the previews know about a file before they open it (the UI thread never waits on a
// file that may sit on a stalled share or an offline cloud drive). A probe worker answers "reached" or not; a
// positive answer stays good for `kYes` s, a negative one for `kNo` s (then the file is asked again).
enum class Reach { Unknown, Yes, No };
class ReachCache {
public:
    static constexpr double kYes = 30.0, kNo = 3.0;
    void  Set(const std::string& path, bool ok, double now_s, int64_t size = -1);
    Reach Get(const std::string& path, double now_s) const;
    // Freeze audit #2: the last answer whatever its age (the missing-sounds checks keep it while a newer one is
    // asked: "unknown yet" never turns a present file missing): false when the path was never answered.
    bool Last(const std::string& path, bool* ok, int64_t* size, double* age_s, double now_s) const;

private:
    struct Entry {
        bool    ok = false;
        double  at = 0.0;
        int64_t size = -1;  // the file's size when reached
    };
    std::map<std::string, Entry> m_;
};

}  // namespace rav
