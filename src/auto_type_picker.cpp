// SPDX-License-Identifier: MIT
//
// See auto_type_picker.h.

#include "auto_type_picker.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "auto_types.h"

namespace rav {

namespace {

// The common types, by value (presentation data, not detection: the type table stays as it is).
constexpr AutoType kCommonTypes[] = {AutoType::Step, AutoType::LiftOff, AutoType::Jump,
                                     AutoType::Land, AutoType::Grab,    AutoType::Release};

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

bool NeedsEscape(unsigned char c)
{
    return c < 0x20 || c == 0x7F || c == '%' || c == '|' || c == ';' || c == ',';
}

int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// %XX decoding; false on a bad escape.
bool Unescape(const std::string& in, std::string* out)
{
    out->clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out->push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size()) return false;
        const int hi = HexVal(in[i + 1]), lo = HexVal(in[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out->push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    return true;
}

std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t                   start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

bool TypeOfWord(const std::string& word, AutoType* t)
{
    for (int i = 0; i < kAutoTypeCount; ++i) {
        const AutoType ti = static_cast<AutoType>(i);
        const char*    w = AutoTypeRow(ti).sound_word;
        if (w && word == w) {
            *t = ti;
            return true;
        }
    }
    return false;
}

}  // namespace

bool AutoTypeCommon(AutoType t)
{
    return std::find(std::begin(kCommonTypes), std::end(kCommonTypes), t) != std::end(kCommonTypes);
}

std::vector<AutoType> AutoPickerOrder(AutoCategory c)
{
    std::vector<AutoType> out = AutoTypesOf(c);
    std::stable_partition(out.begin(), out.end(), [](AutoType t) { return AutoTypeCommon(t); });
    return out;
}

bool AutoTypeMatches(AutoType t, const std::string& query)
{
    const std::string q = Lower(Trim(query));
    if (q.empty()) return true;
    const AutoTypeInfo& row = AutoTypeRow(t);
    std::string         hay = std::string(row.label ? row.label : "") + " " + (row.undo_label ? row.undo_label : "") + " " +
                      AutoCategoryLabel(row.category) + " " + (row.hint ? row.hint : "");
    const std::vector<AutoKind> kinds = AutoKindsOf(t);
    if (!kinds.empty() && AutoKindRow(kinds.front()).marker) hay += std::string(" ") + AutoKindRow(kinds.front()).marker;
    return Lower(hay).find(q) != std::string::npos;
}

std::string CleanAutoPresetName(const std::string& name)
{
    std::string s;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        s.push_back(u < 0x20 || u == 0x7F ? ' ' : c);
    }
    s = Trim(s);
    if (s.size() > kAutoPresetNameMax) {
        size_t n = kAutoPresetNameMax;
        // Never cut inside a UTF-8 character: back up over continuation bytes.
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
        s = Trim(s.substr(0, n));
    }
    return s;
}

std::string EncodeAutoPresets(const std::vector<AutoPreset>& presets)
{
    std::string out;
    int         n = 0;
    for (const AutoPreset& p : presets) {
        const std::string name = CleanAutoPresetName(p.name);
        if (name.empty() || n >= kAutoPresetMax) continue;
        if (n++) out += '|';
        for (char c : name) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (NeedsEscape(u)) {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", u);
                out += buf;
            } else {
                out += c;
            }
        }
        out += ';';
        bool first = true;
        for (int i = 0; i < kAutoTypeCount; ++i) {
            const AutoType t = static_cast<AutoType>(i);
            if (std::find(p.types.begin(), p.types.end(), t) == p.types.end()) continue;
            const char* w = AutoTypeRow(t).sound_word;
            if (!w) continue;
            if (!first) out += ',';
            out += w;
            first = false;
        }
    }
    return out;
}

std::vector<AutoPreset> DecodeAutoPresets(const std::string& value)
{
    std::vector<AutoPreset> out;
    if (value.empty()) return out;
    for (const std::string& rec : Split(value, '|')) {
        if (static_cast<int>(out.size()) >= kAutoPresetMax) break;
        const size_t semi = rec.find(';');
        if (semi == std::string::npos) continue;
        std::string name;
        if (!Unescape(rec.substr(0, semi), &name)) continue;
        name = CleanAutoPresetName(name);
        if (name.empty()) continue;
        AutoPreset p;
        p.name = name;
        bool seen[kAutoTypeCount] = {};
        for (const std::string& w : Split(rec.substr(semi + 1), ',')) {
            AutoType t;
            if (TypeOfWord(Trim(w), &t)) seen[static_cast<int>(t)] = true;
        }
        for (int i = 0; i < kAutoTypeCount; ++i)
            if (seen[i]) p.types.push_back(static_cast<AutoType>(i));
        out.push_back(p);
    }
    return out;
}

AutoPreset AutoPresetFromTicks(const std::string& name, const AutoSettings& s)
{
    AutoPreset p;
    p.name = CleanAutoPresetName(name);
    for (int i = 0; i < kAutoTypeCount; ++i)
        if (s.type[i].on) p.types.push_back(static_cast<AutoType>(i));
    return p;
}

bool AutoPresetMatches(const AutoPreset& p, const AutoSettings& s)
{
    for (int i = 0; i < kAutoTypeCount; ++i) {
        const bool in = std::find(p.types.begin(), p.types.end(), static_cast<AutoType>(i)) != p.types.end();
        if (in != s.type[i].on) return false;
    }
    return true;
}

bool ApplyAutoPreset(const AutoPreset& p, AutoSettings& s)
{
    bool changed = false;
    for (int i = 0; i < kAutoTypeCount; ++i) {
        const bool in = std::find(p.types.begin(), p.types.end(), static_cast<AutoType>(i)) != p.types.end();
        if (s.type[i].on != in) {
            s.type[i].on = in;
            changed = true;
        }
    }
    return changed;
}

}  // namespace rav
