// SPDX-License-Identifier: MIT
//
// Story 13-28 -- the Edit window's LEVEL section: REAPER's normalize modes, the scope (per variation, per
// part, whole sound) and the user's normalize presets. Pure C++17 (no REAPER, no ImGui): the modes table,
// the presets' file format, their load / save (atomic) under a root folder given by the caller (REAPER's
// resource path), the lit chip and the name checks.
//
// The presets are the user's, shared across projects and REAPER sessions: never written in a project nor
// in a .ravlib. File: <root>/ReaAnimViewer/NormalizePresets.txt, a "ravnormpresets 1" header, then one
// preset per line, tab-separated: name, scope word (variation | part | whole), mode word (peak | tp |
// rmsi | lufsi | lufsm | lufss), target. No file = the 3 defaults; an empty list once saved stays empty.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace rav {

// REAPER's normalize modes, in the mockup's order.
enum class NormMode { Peak, TruePeak, RmsI, LufsI, LufsMMax, LufsSMax };
constexpr int kNormModeCount = 6;

// Who gets a gain: each variation, each part of each variation (a Sound divided by a Switch Group), or one
// gain for all (the loudest reaches the target).
enum class NormScope { Variation, Part, Whole };

// The target's range (one value shared by every mode).
constexpr double kNormTargetMin = -60.0;
constexpr double kNormTargetMax = 0.0;

inline const char* NormModeLabel(NormMode m)
{
    switch (m) {
    case NormMode::TruePeak: return "True peak";
    case NormMode::RmsI: return "RMS-I";
    case NormMode::LufsI: return "LUFS-I";
    case NormMode::LufsMMax: return "LUFS-M max";
    case NormMode::LufsSMax: return "LUFS-S max";
    default: return "Peak";
    }
}

inline const char* NormModeUnit(NormMode m)
{
    switch (m) {
    case NormMode::TruePeak: return "dBTP";
    case NormMode::RmsI: return "dB";
    case NormMode::LufsI:
    case NormMode::LufsMMax:
    case NormMode::LufsSMax: return "LUFS";
    default: return "dBFS";
    }
}

// CalculateNormalization's normalizeTo: 0 = LUFS-I, 1 = RMS-I, 2 = peak, 3 = true peak, 4 = LUFS-M max,
// 5 = LUFS-S max.
inline int NormModeReaperIndex(NormMode m)
{
    switch (m) {
    case NormMode::TruePeak: return 3;
    case NormMode::RmsI: return 1;
    case NormMode::LufsI: return 0;
    case NormMode::LufsMMax: return 4;
    case NormMode::LufsSMax: return 5;
    default: return 2;
    }
}

inline const char* NormModeWord(NormMode m)
{
    switch (m) {
    case NormMode::TruePeak: return "tp";
    case NormMode::RmsI: return "rmsi";
    case NormMode::LufsI: return "lufsi";
    case NormMode::LufsMMax: return "lufsm";
    case NormMode::LufsSMax: return "lufss";
    default: return "peak";
    }
}

inline const char* NormScopeWord(NormScope s)
{
    return s == NormScope::Part ? "part" : s == NormScope::Whole ? "whole" : "variation";
}

inline const char* NormScopeLabel(NormScope s)
{
    return s == NormScope::Part ? "Per part" : s == NormScope::Whole ? "Whole sound" : "Per variation";
}

// The scope that acts: "Per part" on an undivided Sound is "Per variation".
inline NormScope EffectiveNormScope(NormScope s, bool divided)
{
    return s == NormScope::Part && !divided ? NormScope::Variation : s;
}

bool NormModeFromWord(const std::string& word, NormMode* out);
bool NormScopeFromWord(const std::string& word, NormScope* out);
// The mode of a stored index (0-5, as kept in the user's prefs); Peak when out of range.
NormMode NormModeFromIndex(int i);
NormScope NormScopeFromIndex(int i);

// The target as shown: "-1", "-14.5" (one decimal at most, no trailing ".0").
std::string FormatNormTarget(double target);

struct NormPreset {
    std::string name;
    NormScope   scope = NormScope::Variation;
    NormMode    mode = NormMode::Peak;
    double      target = -1.0;
};

// The mockup's 3 presets: Steps -1 peak (Per variation, Peak -1), Steps -18 LUFS-S (Per part, LUFS-S max
// -18), Whole -14 LUFS-I (Whole sound, LUFS-I -14). The names use U+2212 (minus sign).
std::vector<NormPreset> DefaultNormPresets();

// The file's text (header + one line per preset) and back. Parse skips a line that does not read (bad
// scope / mode / target, no name, a name already taken); `header_ok` (optional) tells whether the header
// was found.
std::string SerializeNormPresets(const std::vector<NormPreset>& presets);
std::vector<NormPreset> ParseNormPresets(const std::string& text, bool* header_ok = nullptr);

// <root>/ReaAnimViewer/NormalizePresets.txt.
std::string NormPresetsPath(const std::string& root);
// The presets saved under `root`: the defaults when the file does not exist (or `root` is empty). A file that
// exists but cannot be read also gives the defaults, with `read_failed` (optional) set: the caller must then
// not save over it.
std::vector<NormPreset> LoadNormPresets(const std::string& root, bool* read_failed = nullptr);
// Writes them (tmp file + rename: a crash never leaves half a file). False with `err` set on failure.
bool SaveNormPresets(const std::string& root, const std::vector<NormPreset>& presets, std::string* err = nullptr);

// True when the preset, as it would act on this Sound (EffectiveNormScope), equals the current settings:
// same scope and mode, targets within 0.05.
bool NormPresetMatches(const NormPreset& p, NormScope scope, NormMode mode, double target, bool divided);
// The first preset that matches, -1 when none.
int MatchingNormPreset(const std::vector<NormPreset>& presets, NormScope scope, NormMode mode, double target,
                       bool divided);

// "" when `name` (trimmed) can name a preset, else the refusal: "Type a name" (empty), "A preset is
// already named X" (another preset has it, ignoring ASCII case; `except` = the preset being renamed, -1
// none).
std::string NormPresetNameError(const std::vector<NormPreset>& presets, const std::string& name, int except = -1);
// Spaces / tabs / line breaks trimmed at both ends, tabs and line breaks inside turned to spaces (the file
// is tab-separated, one preset per line).
std::string CleanNormPresetName(const std::string& name);

// "Per variation · Peak · -1 dBFS".
std::string NormPresetDescription(const NormPreset& p);

// The Normalize toast: "<label>: N variations normalized to -1 dBFS (Peak)", "... N parts ...",
// "... (LUFS-I, whole sound)"; ", K silent, left as is" appended when regions were skipped. `label` ""
// = the Normalize button (no prefix).
std::string NormalizeToastText(const std::string& label, int done, int skipped, NormScope scope, NormMode mode,
                               double target);
// 13-62: the toast of a Normalize whose regions carry their own (mode, target): NormalizeToastText when they
// all share one (targets within 0.05 dB), else "<label>: N parts normalized, each to its own target" (+ the
// silent count). `targets` = one per region; empty = Peak -1.
std::string NormalizeRegionsToastText(const std::string& label, int done, int skipped, NormScope scope,
                                      const std::vector<std::pair<NormMode, double>>& targets);

}  // namespace rav
