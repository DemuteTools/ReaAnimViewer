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
    std::vector<std::string> path;           // uids from the auditioned node's child to the Sound
};

struct AuditionPlan {
    std::string                root;  // the auditioned node
    std::vector<AuditionVoice> voices;
    std::string                played;  // "Layer 2 › grass_b + Sound A", "nothing" when no voice
    double                     length_ms = 0.0;  // max delay + length
};

// What one press plays (decisions 5-6). A child with no active variation is skipped.
AuditionPlan PlanAudition(const SoundStore& store, const SoundLibrary& lib, const std::string& node_uid,
                          const AuditionPicker& picker);
// The "played: ..." text of these voices (decision 6): "nothing" when none.
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

// The selection Normalize works on: the selected frames (NormalizeFrames keeps the on ones); {} =
// every frame (no selection); {-1} = none when only off frames are selected.
std::vector<int> NormalizeSelection(const std::vector<SoundFrame>& frames, const std::vector<int>& selected);
// Takes mode Normalize: each take has its own file, peak_db[i] = take i's frame peak (non-finite =
// not read: left as is). Per variation: target - own peak; whole: target - the loudest of the set,
// for each. `selection` as NormalizeSelection. Sets `hand`; true when a frame changed.
bool NormalizeTakeFrames(std::vector<SoundFrame>& frames, const std::vector<double>& peak_db, double target_db,
                         bool whole, const std::vector<int>& selection);

// "D → N variations · K off" (decision 7): "D → " dropped when D = N, " · K off" when K = 0;
// "1 variation" singular.
std::string VariationCountText(int detected, const std::vector<SoundFrame>& frames);

}  // namespace rav
