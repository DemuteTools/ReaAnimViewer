// SPDX-License-Identifier: MIT
//
// See tagging_signal.h.

#include "tagging_signal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rav {

SignalUnit UnitOf(const SignalSpec& spec)
{
    const bool angle = IsAngleSignal(spec);
    switch (spec.measure) {
    case Measure::Position: return angle ? SignalUnit::Degree : SignalUnit::Centimetre;
    case Measure::Speed: return angle ? SignalUnit::DegreePerSecond : SignalUnit::MetrePerSecond;
    case Measure::Acceleration: return angle ? SignalUnit::DegreePerSecond2 : SignalUnit::MetrePerSecond2;
    }
    return SignalUnit::Centimetre;
}

const char* UnitLabel(SignalUnit u)
{
    switch (u) {
    case SignalUnit::Centimetre: return "cm";
    case SignalUnit::MetrePerSecond: return "m/s";
    case SignalUnit::MetrePerSecond2: return "m/s\xC2\xB2";
    case SignalUnit::Degree: return "\xC2\xB0";
    case SignalUnit::DegreePerSecond: return "\xC2\xB0/s";
    case SignalUnit::DegreePerSecond2: return "\xC2\xB0/s\xC2\xB2";
    }
    return "";
}

double UnitScale(SignalUnit u)
{
    return u == SignalUnit::Centimetre ? 100.0 : 1.0;
}

int UnitDecimals(SignalUnit u)
{
    switch (u) {
    case SignalUnit::Centimetre: return 1;
    case SignalUnit::MetrePerSecond: return 2;
    case SignalUnit::MetrePerSecond2: return 1;
    case SignalUnit::Degree: return 1;
    case SignalUnit::DegreePerSecond: return 0;
    case SignalUnit::DegreePerSecond2: return 0;
    }
    return 1;
}

double UnitDragStep(SignalUnit u)
{
    switch (u) {
    case SignalUnit::Centimetre: return 0.1;
    case SignalUnit::MetrePerSecond: return 0.01;
    case SignalUnit::MetrePerSecond2: return 0.1;
    case SignalUnit::Degree: return 0.5;
    case SignalUnit::DegreePerSecond: return 5.0;
    case SignalUnit::DegreePerSecond2: return 50.0;
    }
    return 0.1;
}

double ToDisplay(const SignalSpec& spec, double engine_value)
{
    return engine_value * UnitScale(UnitOf(spec));
}

double FromDisplay(const SignalSpec& spec, double display_value)
{
    return display_value / UnitScale(UnitOf(spec));
}

std::string FormatDisplayNumber(const SignalSpec& spec, double engine_value)
{
    double v = ToDisplay(spec, engine_value);
    if (!std::isfinite(v)) v = 0.0;
    const int dec = UnitDecimals(UnitOf(spec));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
    // "-0.0" reads as "0.0".
    std::string s = buf;
    if (s[0] == '-' && s.find_first_not_of("-0.") == std::string::npos) s.erase(0, 1);
    return s;
}

std::string FormatDisplay(const SignalSpec& spec, double engine_value)
{
    const SignalUnit u = UnitOf(spec);
    const std::string n = FormatDisplayNumber(spec, engine_value);
    // Degrees stick to the number ("12.0\xC2\xB0"), other units take a space.
    return u == SignalUnit::Degree ? n + UnitLabel(u) : n + " " + UnitLabel(u);
}

const char* MeasureLabel(Measure m)
{
    switch (m) {
    case Measure::Position: return "Position";
    case Measure::Speed: return "Speed";
    case Measure::Acceleration: return "Acceleration";
    }
    return "?";
}

const char* AxisLabel(Axis a)
{
    switch (a) {
    case Axis::Vertical: return "vertical";
    case Axis::Horizontal: return "horizontal";
    case Axis::Total: return "total";
    case Axis::X: return "X";
    case Axis::Y: return "Y";
    case Axis::Z: return "Z";
    }
    return "?";
}

const char* DirectionLabel(Direction d)
{
    return d == Direction::Below ? "below" : "above";
}

bool IsAngleSignal(const SignalSpec& spec)
{
    return spec.quantity == Quantity::JointAngle || spec.quantity == Quantity::Yaw;
}

const char* PlacementLabel(Placement p)
{
    switch (p) {
    case Placement::Start: return "the start";
    case Placement::Highest: return "the highest point";
    case Placement::Lowest: return "the lowest point";
    }
    return "?";
}

Placement PlacementOf(const Block& b)
{
    if (b.landing == Landing::Crossing) return Placement::Start;
    return b.peak_max ? Placement::Highest : Placement::Lowest;
}

void SetPlacement(Block& b, Placement p, int of)
{
    if (p == Placement::Start) {
        b.landing = Landing::Crossing;
        return;
    }
    b.landing = Landing::PeakOf;
    b.peak_max = (p == Placement::Highest);
    const int n = static_cast<int>(b.conditions.size());
    b.peak_condition = (of >= 0 && of < n) ? of : 0;
}

Condition DefaultCondition()
{
    Condition c;
    c.signal.quantity = Quantity::Point;
    c.signal.bones = {kDefaultConditionBone};
    c.signal.combine = Combine::Single;
    c.signal.reference = Reference::Floor;
    c.signal.measure = Measure::Position;
    c.signal.axis = Axis::Vertical;
    c.dir = Direction::Below;
    c.threshold = 0.05;
    c.margin = 0.02;
    c.auto_threshold = true;
    return c;
}

Block DefaultBlock(const std::string& marker, uint32_t color)
{
    Block b;
    b.marker = marker;
    b.color = color;
    b.conditions = {DefaultCondition()};
    b.cooldown_ms = 250.0;
    return b;
}

void RemoveCondition(Block& b, size_t index)
{
    if (index >= b.conditions.size()) return;
    b.conditions.erase(b.conditions.begin() + static_cast<std::ptrdiff_t>(index));
    if (b.landing != Landing::PeakOf) return;
    const int i = static_cast<int>(index);
    if (b.peak_condition == i) b.peak_condition = 0;
    else if (b.peak_condition > i) --b.peak_condition;
}

void RemapToTracks(std::vector<Block>& bound, std::vector<int>* out_bones)
{
    std::vector<int> bones;
    auto list = [&](std::vector<int>& ids) {
        for (int& id : ids) {
            if (id < 0) continue;
            size_t k = 0;
            while (k < bones.size() && bones[k] != id) ++k;
            if (k == bones.size()) bones.push_back(id);
            id = static_cast<int>(k);
        }
    };
    auto signal = [&](SignalSpec& s) {
        list(s.bones);
        if (s.reference == Reference::Bones) list(s.ref_bones);
    };
    for (Block& b : bound) {
        for (Condition& c : b.conditions) signal(c.signal);
        signal(b.strength_signal);
    }
    if (out_bones) *out_bones = std::move(bones);
}

void CopyAnalysedValues(const std::vector<Block>& analysed, std::vector<Block>& record)
{
    const size_t nb = std::min(analysed.size(), record.size());
    for (size_t b = 0; b < nb; ++b) {
        const Block& a = analysed[b];
        Block& r = record[b];
        const size_t nc = std::min(a.conditions.size(), r.conditions.size());
        for (size_t c = 0; c < nc; ++c) {
            if (!r.conditions[c].auto_threshold) continue;  // Fixed: never touched
            r.conditions[c].threshold = a.conditions[c].threshold;
            r.conditions[c].margin = a.conditions[c].margin;
            r.conditions[c].signal.floor_y = a.conditions[c].signal.floor_y;
            r.conditions[c].signal.bone_floors = a.conditions[c].signal.bone_floors;
        }
        r.strength_signal.floor_y = a.strength_signal.floor_y;
        r.strength_signal.bone_floors = a.strength_signal.bone_floors;
    }
}

std::vector<ClipPass> ClipPasses(const ItemClipMap& m)
{
    std::vector<ClipPass> out;
    const double rate = (m.rate > 0.0 && std::isfinite(m.rate)) ? m.rate : 1.0;
    if (!(m.item_len > 0.0) || !(m.clip_len > 0.0) || !std::isfinite(m.item_pos) || !std::isfinite(m.item_len) ||
        !std::isfinite(m.clip_len))
        return out;
    const double offs = std::isfinite(m.start_offs) ? m.start_offs : 0.0;
    const double item_end = m.item_pos + m.item_len;
    // Source time s(p) = offs + (p - item_pos) * rate; project p(s) = item_pos + (s - offs) / rate.
    auto p_of = [&](double s) { return m.item_pos + (s - offs) / rate; };
    const double s_lo = offs;
    const double s_hi = offs + m.item_len * rate;
    if (!m.loop) {
        const double a = std::max(m.item_pos, p_of(0.0));
        const double b = std::min(item_end, p_of(m.clip_len));
        if (b > a) out.push_back({a, b, offs + (a - m.item_pos) * rate});
        return out;
    }
    const double L = m.clip_len;
    long long k = static_cast<long long>(std::floor(s_lo / L));
    for (int guard = 0; guard < 4096 && static_cast<double>(k) * L < s_hi; ++guard, ++k) {
        const double ks = static_cast<double>(k) * L;
        const double a = std::max(m.item_pos, p_of(ks));
        const double b = std::min(item_end, p_of(ks + L));
        if (b > a) out.push_back({a, b, offs + (a - m.item_pos) * rate - ks});
    }
    return out;
}

std::vector<double> ClipToProjectTimes(const ItemClipMap& m, double clip_t)
{
    std::vector<double> out;
    const double rate = (m.rate > 0.0 && std::isfinite(m.rate)) ? m.rate : 1.0;
    for (const ClipPass& p : ClipPasses(m)) {
        const double t = p.p0 + (clip_t - p.clip0) / rate;
        if (t >= p.p0 - 1e-9 && t < p.p1 - 1e-12) out.push_back(t);
    }
    return out;
}

bool ProjectToClipTime(const ItemClipMap& m, double p, double* out_clip_t)
{
    const double rate = (m.rate > 0.0 && std::isfinite(m.rate)) ? m.rate : 1.0;
    for (const ClipPass& c : ClipPasses(m)) {
        if (p >= c.p0 && p < c.p1) {
            if (out_clip_t) *out_clip_t = c.clip0 + (p - c.p0) * rate;
            return true;
        }
    }
    return false;
}

double SamplePosInPass(const ClipPass& p, double t, double rate, double rate_hz)
{
    const double r = (rate > 0.0 && std::isfinite(rate)) ? rate : 1.0;
    return (p.clip0 + (t - p.p0) * r) * rate_hz;
}

double InterpSample(const std::vector<double>& s, double pos)
{
    if (s.empty()) return 0.0;
    if (!(pos > 0.0)) return s.front();
    const size_t i = static_cast<size_t>(std::floor(pos));
    if (i + 1 >= s.size()) return s.back();
    const double f = pos - static_cast<double>(i);
    return s[i] + (s[i + 1] - s[i]) * f;
}

}  // namespace rav
