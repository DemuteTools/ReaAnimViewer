// SPDX-License-Identifier: MIT
//
// Story 10-8o: the all-trigger bench's pure pieces (tests/trigger_bench.cpp), kept apart so a
// host test pins them (tests/ref_file_test.cpp): the reference file, the old REF markers read as
// references, which row of a bench configuration a reference counts in, and the row match.
//
// Reference file: fixtures/<clip file>.ref.tsv, one expected event per line, tab separated
// (spaces read too), '#' lines and blank lines ignored:
//   kind  side  part  time_s  status  source
//   kind    an auto kind word (auto_detect.h: step, heel, toe, lift, slide, pivot, grab, release,
//           hand_pivot, jump, land, and the part kinds slide_heel, pivot_toe, lift_heel,
//           jump_toe...; a part kind reads as its base kind + that part)
//   side    L / R (story 10-9a-3: jump / land too, the foot that leaves last / touches first;
//           B is still read for them, a reference of no particular foot, scored on a row of
//           its own that no detection fills)
//   part    heel / toe / tip / hand / any
//   time_s  clip time (s)
//   status  draft (Claude, from the motion) / confirmed / deleted (kept, never expected)
//   source  motion / REF (an old REF marker of the csv dump) / antho (the review page)
//
// Configurations: default = every type at the default sensitivity in its default modes (steps
// combined, slides and pivots combined, lift-off / jump / land combined); parts = steps heel / toe
// separate, slides / pivots Each part, lift-off / jump / land Separate (story 10-9a-3). A
// reference counts in a configuration only when it can emit its kind:
//   step / slide / pivot   part any: default (step, slide, pivot); heel: parts (heel,
//                          slide_heel, pivot_heel); toe or tip: parts (toe, slide_toe, pivot_toe)
//   lift, grab, release, hand_pivot, jump, land   both (any part; the lift / jump / land part
//                          kinds score as their type: FoundTarget)
// A row is clip x target kind word x side (story 10-9a-3: jump and land per side too).
//
// Pure C++17, header-only.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "bone_roles.h"
#include "ref_labels.h"

namespace rav {
namespace bench {

struct RefLine {
    std::string kind = "step";  // as written (a part kind is kept as written)
    char        side = 'L';
    std::string part = "any";
    double      time_s = 0.0;
    std::string status = "draft";
    std::string source = "motion";
};

inline bool RefKnownStatus(const std::string& s) { return s == "draft" || s == "confirmed" || s == "deleted"; }
inline bool RefKnownPart(const std::string& p)
{
    return p == "heel" || p == "toe" || p == "tip" || p == "hand" || p == "any";
}

// A kind word as its base kind and part ("slide_heel" -> slide + heel, "heel" -> step + heel,
// "hand_pivot_any" -> hand_pivot); `part` is the line's own part (kept unless the kind names
// one). False for a word that is no auto kind.
inline bool NormaliseRefKind(const std::string& kind, const std::string& part, std::string* base, std::string* out_part)
{
    std::string b = kind, p = part;
    if (kind == "heel" || kind == "toe") {
        b = "step";
        p = kind;
    } else if (kind == "slide_heel" || kind == "slide_heel_longest") {
        b = "slide";
        p = "heel";
    } else if (kind == "slide_toe" || kind == "slide_toe_longest") {
        b = "slide";
        p = "toe";
    } else if (kind == "pivot_heel" || kind == "pivot_heel_longest") {
        b = "pivot";
        p = "heel";
    } else if (kind == "pivot_toe" || kind == "pivot_toe_longest") {
        b = "pivot";
        p = "toe";
    } else if (kind == "hand_pivot_any") {
        b = "hand_pivot";
    } else if (kind == "kick_heel") {  // story 10-9h
        b = "kick";
        p = "heel";
    } else if (kind == "kick_toe") {
        b = "kick";
        p = "toe";
    } else if (kind == "lift_heel" || kind == "jump_heel" || kind == "land_heel") {
        b = kind.substr(0, 4);
        p = "heel";
    } else if (kind == "lift_toe" || kind == "jump_toe" || kind == "land_toe") {
        b = kind.substr(0, 4);
        p = "toe";
    } else if (!(kind == "step" || kind == "lift" || kind == "slide" || kind == "pivot" || kind == "grab" ||
                 kind == "release" || kind == "hand_pivot" || kind == "jump" || kind == "land" ||
                 // story 10-9b: body contacts (sit and fall: side B)
                 kind == "knee" || kind == "elbow" || kind == "sit" || kind == "fall" ||
                 // story 10-9h: the other wave-3 kinds (jolt, spin, spin_end: side B)
                 kind == "kick" || kind == "swing" || kind == "punch" || kind == "clap" || kind == "hand_body" ||
                 kind == "tap" || kind == "drag" || kind == "jolt" || kind == "spin" || kind == "spin_end")) {
        return false;
    }
    if (base) *base = b;
    if (out_part) *out_part = p;
    return true;
}

// Jump and land: the kinds whose reference may say side B (no particular foot, story 10-9a).
// Story 10-9b: sit and fall are always side B (the body); 10-9h: jolt, spin and spin_end too.
inline bool SidelessTarget(const std::string& word)
{
    return word == "jump" || word == "land" || word == "sit" || word == "fall" || word == "jolt" || word == "spin" ||
           word == "spin_end";
}

// One line of a reference file. False (and `err`) for a malformed line; a '#' or blank line is
// false with an empty `err` (nothing to read).
inline bool ParseRefLine(const std::string& raw, RefLine* out, std::string* err)
{
    if (err) err->clear();
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i == line.size() || line[i] == '#') return false;
    std::istringstream       in(line);
    std::vector<std::string> f;
    std::string              w;
    while (in >> w) f.push_back(w);
    if (f.size() != 6) {
        if (err) *err = "expected 6 fields (kind side part time_s status source), got " + std::to_string(f.size());
        return false;
    }
    RefLine r;
    r.kind = f[0];
    if (!NormaliseRefKind(r.kind, "any", nullptr, nullptr)) {
        if (err) *err = "unknown kind '" + f[0] + "'";
        return false;
    }
    std::string base;
    NormaliseRefKind(r.kind, "any", &base, nullptr);
    const bool b_ok = SidelessTarget(base);
    if (f[1].size() != 1 || (f[1][0] != 'L' && f[1][0] != 'R' && !(b_ok && f[1][0] == 'B'))) {
        if (err) *err = std::string("side must be L or R") + (b_ok ? " (or B for jump / land)" : "") + ", got '" + f[1] + "'";
        return false;
    }
    r.side = f[1][0];
    r.part = f[2];
    if (!RefKnownPart(r.part)) {
        if (err) *err = "unknown part '" + f[2] + "'";
        return false;
    }
    char* end = nullptr;
    r.time_s = std::strtod(f[3].c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(r.time_s)) {
        if (err) *err = "time_s is not a number: '" + f[3] + "'";
        return false;
    }
    r.status = f[4];
    if (!RefKnownStatus(r.status)) {
        if (err) *err = "unknown status '" + f[4] + "'";
        return false;
    }
    r.source = f[5];
    if (out) *out = r;
    return true;
}

inline std::string FormatRefLine(const RefLine& r)
{
    char t[32];
    std::snprintf(t, sizeof(t), "%.4f", r.time_s);
    return r.kind + "\t" + std::string(1, r.side) + "\t" + r.part + "\t" + t + "\t" + r.status + "\t" + r.source;
}

constexpr const char* kRefFileHeader = "# kind\tside\tpart\ttime_s\tstatus\tsource";

// The whole file's text (header line, then the lines in order, each ending with '\n').
inline std::string FormatRefFile(const std::vector<RefLine>& lines)
{
    std::string out = std::string(kRefFileHeader) + "\n";
    for (const RefLine& r : lines) out += FormatRefLine(r) + "\n";
    return out;
}

// Parses a file's text. Bad lines go to `errors` ("line 4: ...") and are skipped.
inline std::vector<RefLine> ParseRefFile(const std::string& text, std::vector<std::string>* errors)
{
    std::vector<RefLine> out;
    std::istringstream   in(text);
    std::string          line, err;
    int                  n = 0;
    while (std::getline(in, line)) {
        ++n;
        RefLine r;
        if (ParseRefLine(line, &r, &err)) out.push_back(r);
        else if (!err.empty() && errors) errors->push_back("line " + std::to_string(n) + ": " + err);
    }
    return out;
}

// False when the file cannot be opened (no reference).
inline bool ReadRefFile(const std::string& path, std::vector<RefLine>* lines, std::vector<std::string>* errors)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    *lines = ParseRefFile(ss.str(), errors);
    return true;
}

// ---- Configurations ---------------------------------------------------------------------------

enum class BenchConfig { Default = 0, Parts };
inline const char* BenchConfigName(BenchConfig c) { return c == BenchConfig::Default ? "default" : "parts"; }


// The row kind word a reference counts in, in that configuration ("" = the configuration cannot
// emit it, or the kind is unknown).
inline std::string RefTarget(const RefLine& r, BenchConfig c)
{
    std::string base, part;
    if (!NormaliseRefKind(r.kind, r.part, &base, &part)) return "";
    if (base == "step" || base == "slide" || base == "pivot") {
        if (c == BenchConfig::Default) return part == "any" ? base : "";
        if (part == "heel") return base == "step" ? "heel" : base + "_heel";
        if (part == "toe" || part == "tip") return base == "step" ? "toe" : base + "_toe";
        return "";
    }
    // Story 10-9h: a kick, as a step: one row in Default, the part's (heel, toe) in Parts.
    if (base == "kick") {
        if (c == BenchConfig::Default) return part == "any" ? base : "";
        if (part == "heel") return "kick_heel";
        if (part == "toe" || part == "tip") return "kick_toe";
        return "";
    }
    return base;
}

// The row kind word of a detected block's kind word (the Longest part kinds and the any-hand
// pivot read as their row's; story 10-9a-3: the lift-off / jump / land part kinds as their type).
inline std::string FoundTarget(const std::string& kind_word)
{
    if (kind_word == "lift_heel" || kind_word == "lift_toe") return "lift";
    if (kind_word == "jump_heel" || kind_word == "jump_toe") return "jump";
    if (kind_word == "land_heel" || kind_word == "land_toe") return "land";
    if (kind_word == "slide_heel_longest") return "slide_heel";
    if (kind_word == "slide_toe_longest") return "slide_toe";
    if (kind_word == "pivot_heel_longest") return "pivot_heel";
    if (kind_word == "pivot_toe_longest") return "pivot_toe";
    if (kind_word == "hand_pivot_any") return "hand_pivot";
    return kind_word;
}

// The row's side of a reference: its own (story 10-9a-3: jump and land are scored per side).
inline char RowSide(const std::string&, char side) { return side; }

// ---- Old REF markers ---------------------------------------------------------------------------

// The REF markers of a csv dump as confirmed reference lines (source REF). Event labels through
// ParseEventRefLabel (lift, slide, pivot, grab, release: that side, part any; jump / land: side B).
// Contact labels through the step labels: a role key, or else a bone name whose bone (ref_bone)
// the guessed mapping (`role_bone`, GuessRoleMapping of the dump's `bone_names`) gives a role:
// heel -> step heel, toe -> step toe, toe end -> step tip, hand -> grab hand. Anything else
// (unlabelled REF times, other roles, unknown names) goes to `skipped` ("label: n times").
inline std::vector<RefLine> OldRefLines(const RefTimes& refs, const std::vector<std::string>& bone_names,
                                        const std::vector<int>& role_bone, std::vector<std::string>* skipped)
{
    std::vector<RefLine> out;
    const auto add = [&out](const std::string& kind, char side, const std::string& part, double t) {
        RefLine r;
        r.kind = kind;
        r.side = side;
        r.part = part;
        r.time_s = t;
        r.status = "confirmed";
        r.source = "REF";
        out.push_back(r);
    };
    size_t labelled_contacts = 0;
    for (const auto& kv : refs.by_label) {
        RefEventKind ek;
        char         es = 'L';
        if (ParseEventRefLabel(kv.first, &ek, &es)) {
            static const char* kWords[] = {"lift", "slide", "pivot", "grab", "release", "jump", "land"};
            for (double t : kv.second) add(kWords[static_cast<int>(ek)], es, "any", t);
            continue;
        }
        labelled_contacts += kv.second.size();
        Role role;
        bool have = RoleFromKey(kv.first, &role);
        if (!have) {
            const auto bi = refs.bone.find(kv.first);
            if (bi != refs.bone.end() && !bi->second.empty()) {
                for (size_t b = 0; b < bone_names.size() && !have; ++b) {
                    if (bone_names[b] != bi->second) continue;
                    for (size_t r = 0; r < role_bone.size(); ++r)
                        if (role_bone[r] == static_cast<int>(b)) {
                            role = static_cast<Role>(r);
                            have = true;
                            break;
                        }
                }
            }
        }
        std::string kind, part;
        char        side = 'L';
        if (have) {
            switch (role) {
            case Role::LeftHeel: kind = "step", part = "heel", side = 'L'; break;
            case Role::RightHeel: kind = "step", part = "heel", side = 'R'; break;
            case Role::LeftToe: kind = "step", part = "toe", side = 'L'; break;
            case Role::RightToe: kind = "step", part = "toe", side = 'R'; break;
            case Role::LeftToeEnd: kind = "step", part = "tip", side = 'L'; break;
            case Role::RightToeEnd: kind = "step", part = "tip", side = 'R'; break;
            case Role::LeftHand: kind = "grab", part = "hand", side = 'L'; break;
            case Role::RightHand: kind = "grab", part = "hand", side = 'R'; break;
            default: break;
            }
        }
        if (kind.empty()) {
            if (skipped) skipped->push_back(kv.first + ": " + std::to_string(kv.second.size()) + " times");
            continue;
        }
        for (double t : kv.second) add(kind, side, part, t);
    }
    if (refs.all.size() > labelled_contacts && skipped)
        skipped->push_back("unlabelled REF: " + std::to_string(refs.all.size() - labelled_contacts) + " times");
    std::stable_sort(out.begin(), out.end(), [](const RefLine& a, const RefLine& b) { return a.time_s < b.time_s; });
    return out;
}

// ---- Row match ---------------------------------------------------------------------------------

struct RowRef {
    double t = 0.0;
    bool   confirmed = false;
};

struct RowScore {
    int                 exp_c = 0, exp_d = 0;        // expected (inside the window): confirmed / draft
    int                 found = 0;                   // detections inside the window
    int                 matched_c = 0, matched_d = 0;
    int                 missed_c = 0, missed_d = 0;
    int                 extra = 0;                   // detections matched to no reference
    std::vector<double> err_s;                       // det - ref of each match (s), in reference time order
    std::vector<double> missed_t, extra_t;           // the unmatched references' and detections' times
};

// One row: references (deleted ones never passed in) and detections inside [lo, hi] only, matched
// one-to-one, smallest |err| first, within +-window_s (a tie keeps the earlier reference, then
// the earlier detection).
inline RowScore ScoreRow(std::vector<RowRef> refs, std::vector<double> det, double window_s, double lo, double hi)
{
    RowScore s;
    refs.erase(std::remove_if(refs.begin(), refs.end(), [&](const RowRef& r) { return r.t < lo || r.t > hi; }),
               refs.end());
    det.erase(std::remove_if(det.begin(), det.end(), [&](double t) { return t < lo || t > hi; }), det.end());
    std::stable_sort(refs.begin(), refs.end(), [](const RowRef& a, const RowRef& b) { return a.t < b.t; });
    std::sort(det.begin(), det.end());
    for (const RowRef& r : refs) (r.confirmed ? s.exp_c : s.exp_d)++;
    s.found = static_cast<int>(det.size());
    struct Pair {
        double d;
        size_t a, b;
    };
    std::vector<Pair> pairs;
    for (size_t a = 0; a < refs.size(); ++a)
        for (size_t b = 0; b < det.size(); ++b) {
            const double d = std::fabs(det[b] - refs[a].t);
            if (d <= window_s + 1e-12) pairs.push_back({d, a, b});
        }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.d < y.d; });
    std::vector<int> det_of(refs.size(), -1);
    std::vector<char> used(det.size(), 0);
    for (const Pair& p : pairs) {
        if (det_of[p.a] >= 0 || used[p.b]) continue;
        det_of[p.a] = static_cast<int>(p.b);
        used[p.b] = 1;
    }
    for (size_t a = 0; a < refs.size(); ++a) {
        if (det_of[a] >= 0) {
            (refs[a].confirmed ? s.matched_c : s.matched_d)++;
            s.err_s.push_back(det[static_cast<size_t>(det_of[a])] - refs[a].t);
        } else {
            (refs[a].confirmed ? s.missed_c : s.missed_d)++;
            s.missed_t.push_back(refs[a].t);
        }
    }
    for (size_t b = 0; b < det.size(); ++b)
        if (!used[b]) {
            ++s.extra;
            s.extra_t.push_back(det[b]);
        }
    return s;
}

struct ErrStats {
    int    n = 0;
    double mean_ms = 0.0, sd_ms = 0.0, med_abs_ms = 0.0;
};

inline ErrStats StatsOf(const std::vector<double>& err_s)
{
    ErrStats st;
    st.n = static_cast<int>(err_s.size());
    if (err_s.empty()) return st;
    double sum = 0.0;
    for (double e : err_s) sum += e;
    const double mean = sum / err_s.size();
    double       var = 0.0;
    for (double e : err_s) var += (e - mean) * (e - mean);
    std::vector<double> a;
    for (double e : err_s) a.push_back(std::fabs(e));
    std::sort(a.begin(), a.end());
    const size_t n = a.size();
    st.mean_ms = mean * 1000.0;
    st.sd_ms = n > 1 ? std::sqrt(var / (n - 1)) * 1000.0 : 0.0;
    st.med_abs_ms = (n % 2 ? a[n / 2] : 0.5 * (a[n / 2 - 1] + a[n / 2])) * 1000.0;
    return st;
}

// "exp 3/2  found 3  matched 3/2  missed 0/0  extra 0  mean +8.1  SD 12.0  med 9.0 ms"
// (confirmed/draft; timing over every match, "-" when none).
inline std::string RowText(const RowScore& s)
{
    char         buf[256];
    const ErrStats st = StatsOf(s.err_s);
    if (st.n)
        std::snprintf(buf, sizeof(buf),
                      "exp %d/%d  found %d  matched %d/%d  missed %d/%d  extra %d  mean %+.1f  SD %.1f  med %.1f ms",
                      s.exp_c, s.exp_d, s.found, s.matched_c, s.matched_d, s.missed_c, s.missed_d, s.extra, st.mean_ms,
                      st.sd_ms, st.med_abs_ms);
    else
        std::snprintf(buf, sizeof(buf), "exp %d/%d  found %d  matched %d/%d  missed %d/%d  extra %d  mean -  SD -  med - ms",
                      s.exp_c, s.exp_d, s.found, s.matched_c, s.matched_d, s.missed_c, s.missed_d, s.extra);
    return buf;
}

}  // namespace bench
}  // namespace rav
