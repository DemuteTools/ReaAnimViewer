// SPDX-License-Identifier: MIT
//
// The saved state of the RAV video FX (Epic 11, Story 11-3), shared by both binaries:
// rav_video_fx.clap writes/reads it as its CLAP state (saved with the project, in
// REAPER's FX presets), reaper_animviewer.dll reads it and sends changes through
// REAPER's "clap_chunk" named config (video_shots.cpp).
//
// Text, UTF-8, one record per line; a free-text name always comes last on its line.
//   RAVVFX 1
//   kind full                        full | preset (camera values only) | meta (no values)
//   p 0.5955 0.5966 0.6834 0.5 0.5 0.5   the six camera values (video_camera_params.h)
//   override 1080 1920               output size override, 0 0 = project size
//   shot 12.5 Close-up               a shot name, keyed by the shot's time in seconds
//   angle 0.5 0.6 0.68 0.5 0.5 0.5 Front   a saved angle: six values + name
// Unknown records are skipped, so a later version can add some. Numbers are written
// and read in the "C" locale (each binary has its own static CRT and never changes it).
//
// Header-only and dependency-free (no REAPER, CLAP or glm).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "video_camera_params.h"

namespace rav {

constexpr const char kVideoFxStateMagic[] = "RAVVFX ";
constexpr size_t kVideoFxMaxNames  = 4096;
constexpr size_t kVideoFxMaxAngles = 1024;
constexpr size_t kVideoFxMaxName   = 128;  // bytes
constexpr int    kVideoFxMaxSize   = 8192;  // output override, pixels

enum class VideoFxStateKind { Full, Preset, Meta };

struct VideoFxShotName {
    double time = 0.0;
    std::string name;
};

struct VideoFxAngle {
    std::string name;
    double values[vcam::kParamCount] = {};
};

struct VideoFxState {
    VideoFxStateKind kind = VideoFxStateKind::Full;
    bool has_values = false;  // a "p" record was read (always written by Full / Preset)
    double values[vcam::kParamCount] = {};
    int override_width = 0;  // 0 x 0 = the project's video size
    int override_height = 0;
    std::vector<VideoFxShotName> shot_names;
    std::vector<VideoFxAngle> angles;

    VideoFxState()
    {
        for (int p = 0; p < vcam::kParamCount; ++p) values[p] = vcam::DefaultNorm(p);
    }
};

// One line of free text: control characters become spaces, outer spaces go, the
// length is capped (on a UTF-8 boundary).
inline std::string CleanVideoFxName(const std::string& in)
{
    std::string s(in);
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    }
    const size_t b = s.find_first_not_of(' ');
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(' ');
    s = s.substr(b, e - b + 1);
    if (s.size() > kVideoFxMaxName) {
        size_t cut = kVideoFxMaxName;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;  // not inside a UTF-8 sequence
        s.resize(cut);
    }
    return s;
}

inline bool VideoFxOverrideValid(int w, int h)
{
    return w > 0 && h > 0 && w <= kVideoFxMaxSize && h <= kVideoFxMaxSize;
}

inline std::string SerializeVideoFxState(const VideoFxState& st)
{
    std::string out = "RAVVFX 1\n";
    char line[512];
    const char* kind = st.kind == VideoFxStateKind::Preset ? "preset"
                     : st.kind == VideoFxStateKind::Meta   ? "meta"
                                                           : "full";
    out += "kind ";
    out += kind;
    out += '\n';
    if (st.kind != VideoFxStateKind::Meta) {
        std::snprintf(line, sizeof(line), "p %.17g %.17g %.17g %.17g %.17g %.17g\n", st.values[0], st.values[1],
                      st.values[2], st.values[3], st.values[4], st.values[5]);
        out += line;
    }
    if (st.kind == VideoFxStateKind::Preset) return out;

    const bool ov = VideoFxOverrideValid(st.override_width, st.override_height);
    std::snprintf(line, sizeof(line), "override %d %d\n", ov ? st.override_width : 0, ov ? st.override_height : 0);
    out += line;
    for (const VideoFxShotName& n : st.shot_names) {
        const std::string name = CleanVideoFxName(n.name);
        if (name.empty() || !std::isfinite(n.time)) continue;
        std::snprintf(line, sizeof(line), "shot %.17g ", n.time);
        out += line;
        out += name;
        out += '\n';
    }
    for (const VideoFxAngle& a : st.angles) {
        const std::string name = CleanVideoFxName(a.name);
        if (name.empty()) continue;
        std::snprintf(line, sizeof(line), "angle %.17g %.17g %.17g %.17g %.17g %.17g ", a.values[0], a.values[1],
                      a.values[2], a.values[3], a.values[4], a.values[5]);
        out += line;
        out += name;
        out += '\n';
    }
    return out;
}

namespace detail {

// Reads `count` doubles from `s`, advancing it. False if one is missing.
inline bool ReadDoubles(const char*& s, double* out, int count)
{
    for (int i = 0; i < count; ++i) {
        char* end = nullptr;
        const double v = std::strtod(s, &end);
        if (end == s) return false;
        out[i] = v;
        s = end;
    }
    return true;
}

// The rest of the line after one separating space.
inline std::string RestOfLine(const char* s)
{
    if (*s == ' ') ++s;
    return CleanVideoFxName(s);
}

}  // namespace detail

// Parses a state blob. The blob may sit inside a larger buffer (REAPER's chunk): the
// parse starts at the first "RAVVFX " and stops at the end or at a NUL byte. False
// (and `out` untouched) when there is no magic.
inline bool ParseVideoFxState(const char* data, size_t size, VideoFxState* out)
{
    if (!data || !out || size < sizeof(kVideoFxStateMagic) - 1) return false;
    const size_t magic_len = sizeof(kVideoFxStateMagic) - 1;
    size_t start = std::string::npos;
    for (size_t i = 0; i + magic_len <= size; ++i) {
        if (std::memcmp(data + i, kVideoFxStateMagic, magic_len) == 0) {
            start = i;
            break;
        }
    }
    if (start == std::string::npos) return false;
    size_t end = start;
    while (end < size && data[end] != '\0') ++end;
    const std::string text(data + start, end - start);

    VideoFxState st;
    size_t pos = 0;
    bool first = true;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) {  // "RAVVFX <version>": any version, unknown records are skipped
            first = false;
            continue;
        }
        const size_t sp = line.find(' ');
        const std::string key = line.substr(0, sp);
        const char* rest = line.c_str() + (sp == std::string::npos ? line.size() : sp);
        if (key == "kind") {
            const std::string k = detail::RestOfLine(rest);
            st.kind = k == "preset" ? VideoFxStateKind::Preset
                    : k == "meta"   ? VideoFxStateKind::Meta
                                    : VideoFxStateKind::Full;
        } else if (key == "p") {
            double v[vcam::kParamCount];
            if (detail::ReadDoubles(rest, v, vcam::kParamCount)) {
                for (int p = 0; p < vcam::kParamCount; ++p) st.values[p] = vcam::Sanitize(p, v[p]);
                st.has_values = true;
            }
        } else if (key == "override") {
            double v[2];
            // Range-checked as doubles first: casting an out-of-range double to int is UB.
            if (detail::ReadDoubles(rest, v, 2) && v[0] >= 0.0 && v[0] <= kVideoFxMaxSize && v[1] >= 0.0 &&
                v[1] <= kVideoFxMaxSize && VideoFxOverrideValid(static_cast<int>(v[0]), static_cast<int>(v[1]))) {
                st.override_width = static_cast<int>(v[0]);
                st.override_height = static_cast<int>(v[1]);
            }
        } else if (key == "shot") {
            double t = 0.0;
            if (detail::ReadDoubles(rest, &t, 1) && std::isfinite(t) && st.shot_names.size() < kVideoFxMaxNames) {
                VideoFxShotName n;
                n.time = t;
                n.name = detail::RestOfLine(rest);
                if (!n.name.empty()) st.shot_names.push_back(n);
            }
        } else if (key == "angle") {
            VideoFxAngle a;
            if (detail::ReadDoubles(rest, a.values, vcam::kParamCount) && st.angles.size() < kVideoFxMaxAngles) {
                for (int p = 0; p < vcam::kParamCount; ++p) a.values[p] = vcam::Sanitize(p, a.values[p]);
                a.name = detail::RestOfLine(rest);
                if (!a.name.empty()) st.angles.push_back(a);
            }
        }
    }
    *out = st;
    return true;
}

// Applies a loaded blob to the current state, by the blob's kind (or `force_preset`
// when the host says the load is a preset): preset = camera values only, meta = all
// but the camera values, full = everything. Returns true when the camera values changed.
inline bool MergeVideoFxState(const VideoFxState& in, bool force_preset, VideoFxState* cur)
{
    const VideoFxStateKind kind = force_preset ? VideoFxStateKind::Preset : in.kind;
    bool values_changed = false;
    if (kind != VideoFxStateKind::Meta && in.has_values) {
        for (int p = 0; p < vcam::kParamCount; ++p) {
            if (cur->values[p] != in.values[p]) values_changed = true;
            cur->values[p] = in.values[p];
        }
    }
    if (kind != VideoFxStateKind::Preset) {
        cur->override_width = in.override_width;
        cur->override_height = in.override_height;
        cur->shot_names = in.shot_names;
        cur->angles = in.angles;
    }
    cur->kind = VideoFxStateKind::Full;
    cur->has_values = true;
    return values_changed;
}

}  // namespace rav
