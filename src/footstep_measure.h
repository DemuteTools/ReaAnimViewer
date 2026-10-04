// SPDX-License-Identifier: MIT
//
// Measuring the Footsteps preset on one item (Epic 10, spike 10-0): the part shared by
// the REAPER action (detection_measure.cpp) and the offline evaluator
// (tests/detection_eval.cpp), so both run the exact same code and print the same report.
//
// From the skeleton's bone names, a track sampler, the REF times and the visible window
// (all clip time): map roles, bind the preset, sample the bones it needs, Analyse, Detect,
// match against REF.
//
// Pure C++17: no REAPER, no Windows. Host-tested (tests/bone_events_test.cpp).

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "bone_events.h"
#include "bone_presets.h"

namespace rav {

constexpr double kDetectRateHz = 240.0;
constexpr double kMatchWindowS = 0.150;

struct ItemMeasure {
    std::string         skipped;  // non-empty = not measured, with the reason
    std::vector<Block>  blocks;   // as analysed
    std::vector<Event>  events;
    std::vector<double> det;      // event times (clip time)
    MatchResult         match;
};

// Returns one track per bone index, in that order (empty = could not sample).
using TrackSampler = std::function<std::vector<BoneTrack>(const std::vector<int>& bone_indices)>;

ItemMeasure MeasureFootsteps(const std::vector<std::string>& bone_names, const TrackSampler& sample,
                             const std::vector<double>& refs, double lo, double hi, const FootstepsParams& prm);

// ---- The report (the action's console and the evaluator's stdout) ----------------------
std::string ParamsText(const FootstepsParams& prm);
std::string MatchText(const MatchResult& m);
std::string TimesText(const std::vector<double>& t);
// A measured item: its match line (with `note` appended), thresholds, unmatched times and
// one line per detection (the nearest REF and when height / knee / yaw came true).
std::string ItemReport(const std::string& label, const ItemMeasure& m, const std::vector<double>& refs,
                       const std::string& note);
std::string SkippedReport(const std::string& label, const std::string& reason);
// The TOTAL line with the GO / NO-GO verdict (NFR-AT1).
std::string TotalReport(const std::vector<MatchResult>& measured, const FootstepsParams& prm);
// One line of JSON per item (for scripted parameter search).
std::string ItemJson(const std::string& label, const ItemMeasure& m);

}  // namespace rav
