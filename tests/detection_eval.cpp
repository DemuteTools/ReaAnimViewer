// SPDX-License-Identifier: MIT
//
// Offline evaluator for footstep detection (Epic 10, spike 10-0). Runs the real engine
// (footstep_measure.h: the same code as "RAV: Measure detection against reference
// markers") on the bone-track dumps that action writes to RAV_detection_dump/*.csv, and
// prints the same report. For tuning FootstepsParams on tagged clips without REAPER.
//
//   detection_eval [--param=value ...] [--json] file.csv...
//
// Params (FootstepsParams): --height=<% of gap> --knee=<x p95> --yaw=<deg/s>
//   --yaw-margin=<deg/s> --hold=<ms> --cooldown=<ms> --offset=<ms> --floor=bone|foot
//   --floor-pct=<pct> --margin-ratio=<r> --smooth=<ms> --sensitivity=<0-1> --edge=<ms>
// --json: one JSON line per item, then one for the TOTAL (for scripted parameter search).
//
// The CSV: '# item=<label>', '# rate_hz=<hz>', '# visible=<lo>,<hi>', '# ref=<t>,<t>...',
// '# parent=...' (ignored), then the header 't,<bone>.x,<bone>.y,<bone>.z,...' and one row
// per sample (metres, model Y up).
// Positions only: a rotation condition (q=rot, 10-4 follow-up) cannot be evaluated from a
// dump. This tool only runs the built-in Footsteps preset, which has no rotation condition;
// MeasureFootsteps keeps a defensive skip for one, and any rotation signal on position-only
// tracks evaluates empty and never runs.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "footstep_measure.h"

using namespace rav;

namespace {

std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

bool ToDouble(const std::string& s, double* out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    *out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

struct Clip {
    std::string            label;
    double                 rate = 240.0;
    bool                   has_visible = false;
    double                 lo = 0.0, hi = 0.0;
    std::vector<double>    refs;
    std::vector<std::string> names;
    std::vector<BoneTrack> tracks;
};

bool ReadClip(const std::string& path, Clip& c, std::string* err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *err = "cannot open";
        return false;
    }
    const size_t slash = path.find_last_of("/\\");
    c.label = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string line;
    bool header = false;
    size_t row = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            std::string kv = line.substr(1);
            while (!kv.empty() && kv[0] == ' ') kv.erase(0, 1);
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "item") {
                c.label = v;
            } else if (k == "rate_hz") {
                if (!ToDouble(v, &c.rate) || !(c.rate > 0.0)) {
                    *err = "bad rate_hz";
                    return false;
                }
            } else if (k == "visible") {
                const auto p = Split(v, ',');
                c.has_visible = p.size() == 2 && ToDouble(p[0], &c.lo) && ToDouble(p[1], &c.hi);
            } else if (k == "ref") {
                for (const std::string& t : Split(v, ',')) {
                    double x;
                    if (ToDouble(t, &x)) c.refs.push_back(x);
                }
            }
            continue;
        }
        const auto cols = Split(line, ',');
        if (!header) {
            if (cols.empty() || cols[0] != "t" || (cols.size() - 1) % 3 != 0) {
                *err = "bad header (expected t,<bone>.x,<bone>.y,<bone>.z,...)";
                return false;
            }
            for (size_t i = 1; i < cols.size(); i += 3) {
                const std::string& n = cols[i];
                c.names.push_back(n.size() > 2 && n.compare(n.size() - 2, 2, ".x") == 0 ? n.substr(0, n.size() - 2) : n);
            }
            c.tracks.assign(c.names.size(), BoneTrack{});
            for (BoneTrack& t : c.tracks) t.rate_hz = c.rate;
            header = true;
            continue;
        }
        ++row;
        if (cols.size() != 1 + 3 * c.names.size()) {
            *err = "row " + std::to_string(row) + ": wrong column count";
            return false;
        }
        for (size_t b = 0; b < c.names.size(); ++b) {
            Vec3d p;
            if (!ToDouble(cols[1 + 3 * b], &p.x) || !ToDouble(cols[2 + 3 * b], &p.y) ||
                !ToDouble(cols[3 + 3 * b], &p.z)) {
                *err = "row " + std::to_string(row) + ": not a number";
                return false;
            }
            c.tracks[b].pos.push_back(p);
        }
    }
    if (!header || c.tracks.empty() || c.tracks[0].pos.empty()) {
        *err = "no samples";
        return false;
    }
    if (!c.has_visible) {
        c.lo = 0.0;
        c.hi = static_cast<double>(c.tracks[0].pos.size() - 1) / c.rate;
    }
    return true;
}

// --name=value into the params. False on an unknown name or a bad value.
bool SetParam(const std::string& name, const std::string& value, FootstepsParams& p)
{
    if (name == "floor") {
        if (value == "bone") p.per_bone_floor = true;
        else if (value == "foot") p.per_bone_floor = false;
        else return false;
        return true;
    }
    double v;
    if (!ToDouble(value, &v)) return false;
    if (name == "height") p.height_fraction = v / 100.0;
    else if (name == "knee") p.knee_fraction = v;
    else if (name == "yaw") p.yaw_limit_dps = v;
    else if (name == "yaw-margin") p.yaw_margin_dps = v;
    else if (name == "hold") p.min_hold_ms = v;
    else if (name == "cooldown") p.cooldown_ms = v;
    else if (name == "offset") p.offset_ms = v;
    else if (name == "floor-pct") p.floor_percentile = v;
    else if (name == "margin-ratio") p.margin_ratio = v;
    else if (name == "smooth") p.smooth_ms = v;
    else if (name == "sensitivity") p.sensitivity = v;
    else if (name == "edge") p.edge_margin_ms = v;
    else return false;
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    FootstepsParams prm;
    bool json = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            json = true;
        } else if (a.compare(0, 2, "--") == 0) {
            const size_t eq = a.find('=');
            if (eq == std::string::npos || !SetParam(a.substr(2, eq - 2), a.substr(eq + 1), prm)) {
                std::fprintf(stderr, "detection_eval: bad option %s\n", a.c_str());
                return 2;
            }
        } else {
            files.push_back(a);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: detection_eval [--param=value ...] [--json] file.csv...\n");
        return 2;
    }

    if (!json)
        std::printf("=== detection_eval ===\nFootsteps preset + Analyse, match window +-%g ms, source time\n",
                    kMatchWindowS * 1000.0);
    std::vector<MatchResult> measured;
    for (size_t f = 0; f < files.size(); ++f) {
        Clip c;
        std::string err;
        const std::string label = "#" + std::to_string(f + 1) + " ";
        ItemMeasure m;
        if (!ReadClip(files[f], c, &err)) {
            m.skipped = "could not read " + files[f] + ": " + err;
        } else if (c.refs.empty()) {
            m.skipped = "no REF times in the file";
        } else {
            m = MeasureFootsteps(
                c.names,
                [&](const std::vector<int>& idx) {
                    std::vector<BoneTrack> t;
                    for (int i : idx) t.push_back(c.tracks.at(static_cast<size_t>(i)));
                    return t;
                },
                c.refs, c.lo, c.hi, prm);
        }
        const std::string name = c.label.empty() ? files[f] : c.label;
        if (m.skipped.empty()) measured.push_back(m.match);
        if (json) {
            std::printf("%s\n", ItemJson(name, m).c_str());
        } else if (!m.skipped.empty()) {
            std::printf("%s", SkippedReport(label + name, m.skipped).c_str());
        } else {
            std::printf("%s", ItemReport(label + name, m, c.refs, "").c_str());
        }
    }
    if (json) {
        ItemMeasure total;
        total.match = SumMatches(measured);
        if (measured.empty()) total.skipped = "nothing measured";
        std::printf("%s\n", ItemJson("TOTAL", total).c_str());
    } else {
        std::printf("%s", TotalReport(measured, prm).c_str());
    }
    return 0;
}
