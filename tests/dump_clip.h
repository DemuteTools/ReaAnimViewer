// SPDX-License-Identifier: MIT
//
// Story 10-8o: the bone-track dump reader shared by tests/detection_eval.cpp and
// tests/trigger_bench.cpp (moved out of detection_eval unchanged, plus
// Clip::parent, read from '# parent=', which detection_eval ignores). The CSV: '# item=<label>',
// '# rate_hz=<hz>', '# visible=<lo>,<hi>', the REF lines (ref_labels.h), '# parent=<i>,<i>...'
// (each bone's parent index, -1 = none; kept in Clip::parent, which detection_eval ignores),
// then 't,<bone>.x,<bone>.y,<bone>.z[,<bone>.qw,.qx,.qy,.qz],...' and one row per sample.

#pragma once

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "bone_events.h"
#include "ref_labels.h"

namespace rav {
namespace dump {

inline std::vector<std::string> Split(const std::string& s, char sep)
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

inline bool ToDouble(const std::string& s, double* out)
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
    RefTimes               refs;
    std::vector<std::string> names;
    std::vector<BoneTrack> tracks;
    std::vector<int>       parent;  // '# parent=' (empty when absent); detection_eval ignores it
};

inline bool ReadClip(const std::string& path, Clip& c, std::string* err)
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
    size_t row = 0, stride = 3;
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
            } else if (k == "parent") {
                c.parent.clear();
                for (const std::string& s : Split(v, ',')) {
                    double d;
                    c.parent.push_back(ToDouble(s, &d) ? static_cast<int>(d) : -1);
                }
            } else if (k == "visible") {
                const auto p = Split(v, ',');
                c.has_visible = p.size() == 2 && ToDouble(p[0], &c.lo) && ToDouble(p[1], &c.hi);
            } else {
                ReadRefHeader(k, v, &c.refs);  // ref, ref.<label>, ref_bone.<label>; others ignored
            }
            continue;
        }
        const auto cols = Split(line, ',');
        if (!header) {
            // 3 columns per bone, or 7 with its rotation (.qw,.qx,.qy,.qz after .z, story 10-8d).
            auto ends = [](const std::string& n, const char* tail) {
                const size_t k = std::strlen(tail);
                return n.size() >= k && n.compare(n.size() - k, k, tail) == 0;
            };
            stride = cols.size() > 4 && ends(cols[4], ".qw") ? 7 : 3;
            if (cols.empty() || cols[0] != "t" || (cols.size() - 1) % stride != 0) {
                *err = "bad header (expected t,<bone>.x,<bone>.y,<bone>.z[,<bone>.qw,.qx,.qy,.qz],...)";
                return false;
            }
            for (size_t i = 1; i < cols.size(); i += stride) {
                const std::string& n = cols[i];
                c.names.push_back(n.size() > 2 && n.compare(n.size() - 2, 2, ".x") == 0 ? n.substr(0, n.size() - 2) : n);
            }
            c.tracks.assign(c.names.size(), BoneTrack{});
            for (BoneTrack& t : c.tracks) t.rate_hz = c.rate;
            header = true;
            continue;
        }
        ++row;
        if (cols.size() != 1 + stride * c.names.size()) {
            *err = "row " + std::to_string(row) + ": wrong column count";
            return false;
        }
        for (size_t b = 0; b < c.names.size(); ++b) {
            const size_t k = 1 + stride * b;
            Vec3d        p;
            if (!ToDouble(cols[k], &p.x) || !ToDouble(cols[k + 1], &p.y) || !ToDouble(cols[k + 2], &p.z)) {
                *err = "row " + std::to_string(row) + ": not a number";
                return false;
            }
            c.tracks[b].pos.push_back(p);
            if (stride == 7) {
                Quatd q;
                if (!ToDouble(cols[k + 3], &q.w) || !ToDouble(cols[k + 4], &q.x) || !ToDouble(cols[k + 5], &q.y) ||
                    !ToDouble(cols[k + 6], &q.z)) {
                    *err = "row " + std::to_string(row) + ": not a number";
                    return false;
                }
                c.tracks[b].rot_world.push_back(q);
            }
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

}  // namespace dump
}  // namespace rav
