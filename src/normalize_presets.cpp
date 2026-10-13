// SPDX-License-Identifier: MIT
//
// See normalize_presets.h (story 13-28).

#include "normalize_presets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace rav {
namespace {

namespace fs = std::filesystem;

constexpr char kHeader[] = "ravnormpresets";
constexpr int  kVersion = 1;

std::string LowerAscii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::vector<std::string> SplitTabs(const std::string& line)
{
    std::vector<std::string> out;
    size_t                   at = 0;
    for (;;) {
        const size_t t = line.find('\t', at);
        out.push_back(line.substr(at, t == std::string::npos ? std::string::npos : t - at));
        if (t == std::string::npos) break;
        at = t + 1;
    }
    return out;
}

bool ReadTarget(const std::string& s, double* out)
{
    if (s.empty()) return false;
    char*        end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v)) return false;
    *out = std::min(kNormTargetMax, std::max(kNormTargetMin, v));
    return true;
}

bool ReadFileText(const fs::path& p, std::string* out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return false;
    *out = ss.str();
    return true;
}

bool WriteFileText(const fs::path& p, const std::string& text, std::string* err)
{
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err) *err = "Cannot write " + p.u8string() + ".";
            return false;
        }
        out << text;
        out.flush();
        const bool written = static_cast<bool>(out);
        out.close();
        if (!written || out.fail()) {
            if (err) *err = "Cannot write " + p.u8string() + ".";
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (err) *err = "Cannot write " + p.u8string() + ".";
        return false;
    }
    return true;
}

}  // namespace

bool NormModeFromWord(const std::string& word, NormMode* out)
{
    for (int i = 0; i < kNormModeCount; ++i) {
        const NormMode m = static_cast<NormMode>(i);
        if (word == NormModeWord(m)) {
            *out = m;
            return true;
        }
    }
    return false;
}

bool NormScopeFromWord(const std::string& word, NormScope* out)
{
    for (NormScope s : {NormScope::Variation, NormScope::Part, NormScope::Whole}) {
        if (word == NormScopeWord(s)) {
            *out = s;
            return true;
        }
    }
    return false;
}

NormMode NormModeFromIndex(int i)
{
    return i >= 0 && i < kNormModeCount ? static_cast<NormMode>(i) : NormMode::Peak;
}

NormScope NormScopeFromIndex(int i)
{
    return i == 1 ? NormScope::Part : i == 2 ? NormScope::Whole : NormScope::Variation;
}

std::string FormatNormTarget(double target)
{
    double r = std::round(target * 10.0) / 10.0;
    if (r == 0.0) r = 0.0;  // no "-0"
    char buf[32];
    if (std::fabs(r - std::round(r)) < 1e-9) std::snprintf(buf, sizeof(buf), "%.0f", r);
    else std::snprintf(buf, sizeof(buf), "%.1f", r);
    return buf;
}

std::vector<NormPreset> DefaultNormPresets()
{
    return {
        {"Steps \xE2\x88\x92" "1 peak", NormScope::Variation, NormMode::Peak, -1.0},
        {"Steps \xE2\x88\x92" "18 LUFS-S", NormScope::Part, NormMode::LufsSMax, -18.0},
        {"Whole \xE2\x88\x92" "14 LUFS-I", NormScope::Whole, NormMode::LufsI, -14.0},
    };
}

std::string SerializeNormPresets(const std::vector<NormPreset>& presets)
{
    std::string out = std::string(kHeader) + " " + std::to_string(kVersion) + "\n";
    for (const NormPreset& p : presets) {
        char num[32];
        std::snprintf(num, sizeof(num), "%.10g", p.target);
        out += CleanNormPresetName(p.name) + "\t" + NormScopeWord(p.scope) + "\t" + NormModeWord(p.mode) + "\t" + num + "\n";
    }
    return out;
}

std::vector<NormPreset> ParseNormPresets(const std::string& text, bool* header_ok)
{
    std::vector<NormPreset> out;
    bool                    header = false;
    std::istringstream      in(text);
    std::string             line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.find('\t') == std::string::npos && line.compare(0, sizeof(kHeader) - 1, kHeader) == 0 &&
            (line.size() == sizeof(kHeader) - 1 || line[sizeof(kHeader) - 1] == ' ')) {  // a preset line has tabs
            header = true;
            continue;
        }
        const std::vector<std::string> f = SplitTabs(line);
        if (f.size() < 4) continue;
        NormPreset p;
        p.name = CleanNormPresetName(f[0]);
        if (p.name.empty() || !NormScopeFromWord(f[1], &p.scope) || !NormModeFromWord(f[2], &p.mode) ||
            !ReadTarget(f[3], &p.target))
            continue;
        if (!NormPresetNameError(out, p.name).empty()) continue;  // a name already taken
        out.push_back(p);
    }
    if (header_ok) *header_ok = header;
    return out;
}

std::string NormPresetsPath(const std::string& root)
{
    return (fs::u8path(root) / "ReaAnimViewer" / "NormalizePresets.txt").u8string();
}

std::vector<NormPreset> LoadNormPresets(const std::string& root, bool* read_failed)
{
    if (read_failed) *read_failed = false;
    if (root.empty()) return DefaultNormPresets();
    const fs::path  p = fs::u8path(NormPresetsPath(root));
    std::error_code ec;
    const bool      there = fs::exists(p, ec);
    if (!there && !ec) return DefaultNormPresets();
    std::string text;
    if (ec || !fs::is_regular_file(p, ec) || !ReadFileText(p, &text)) {
        if (read_failed) *read_failed = true;  // the user's file is there but unread: never overwrite it
        return DefaultNormPresets();
    }
    return ParseNormPresets(text);
}

bool SaveNormPresets(const std::string& root, const std::vector<NormPreset>& presets, std::string* err)
{
    if (root.empty()) {
        if (err) *err = "REAPER's resource folder is unknown.";
        return false;
    }
    return WriteFileText(fs::u8path(NormPresetsPath(root)), SerializeNormPresets(presets), err);
}

bool NormPresetMatches(const NormPreset& p, NormScope scope, NormMode mode, double target, bool divided)
{
    return EffectiveNormScope(p.scope, divided) == EffectiveNormScope(scope, divided) && p.mode == mode &&
           std::fabs(p.target - target) <= 0.05 + 1e-9;
}

int MatchingNormPreset(const std::vector<NormPreset>& presets, NormScope scope, NormMode mode, double target,
                       bool divided)
{
    for (size_t i = 0; i < presets.size(); ++i)
        if (NormPresetMatches(presets[i], scope, mode, target, divided)) return static_cast<int>(i);
    return -1;
}

std::string CleanNormPresetName(const std::string& name)
{
    std::string s = name;
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}

std::string NormPresetNameError(const std::vector<NormPreset>& presets, const std::string& name, int except)
{
    const std::string n = CleanNormPresetName(name);
    if (n.empty()) return "Type a name";
    const std::string low = LowerAscii(n);
    for (size_t i = 0; i < presets.size(); ++i)
        if (static_cast<int>(i) != except && LowerAscii(presets[i].name) == low)
            return "A preset is already named " + presets[i].name;
    return std::string();
}

std::string NormPresetDescription(const NormPreset& p)
{
    return std::string(NormScopeLabel(p.scope)) + " \xC2\xB7 " + NormModeLabel(p.mode) + " \xC2\xB7 " +
           FormatNormTarget(p.target) + " " + NormModeUnit(p.mode);
}

std::string NormalizeToastText(const std::string& label, int done, int skipped, NormScope scope, NormMode mode,
                               double target)
{
    std::string s = label.empty() ? std::string() : label + ": ";
    s += std::to_string(done) + " ";
    if (scope == NormScope::Part) s += done == 1 ? "part" : "parts";
    else s += done == 1 ? "variation" : "variations";
    s += " normalized to " + FormatNormTarget(target) + " " + NormModeUnit(mode) + " (" + NormModeLabel(mode);
    if (scope == NormScope::Whole) s += ", whole sound";
    s += ")";
    if (skipped > 0) s += ", " + std::to_string(skipped) + " silent, left as is";
    return s;
}

}  // namespace rav
