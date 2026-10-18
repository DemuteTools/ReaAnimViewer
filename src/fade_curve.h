#pragma once
// 13-75 (Antho 2026-10-19): REAPER's fade curves, exactly, for what RAV draws, plays and places.
//
// A REAPER item fade is a shape (C_FADEINSHAPE / C_FADEOUTSHAPE, 0..6) and a curvature (D_FADEINDIR /
// D_FADEOUTDIR, -1..1). Measured in REAPER 7.81 (an item of constant 1.0 read back through a track audio
// accessor, every shape at curvatures -1, -0.75, -0.5, -0.25, 0, 0.1, 0.25, 0.5, 0.75, 1; spec 13-75):
//  - shapes 0..4 are one family: only the curvature counts (1..4 are names for curvatures, see kFadePresets). The
//    fade-in gain at x in [0, 1] is the warp W(x, c): c = 0 linear, 0.5 -> 1 - (1 - x)^2, 1 -> 1 - (1 - x)^4,
//    -0.5 -> x^2, -1 -> x^4, linear blends of the two neighbours in between;
//  - shape 5 (slight S-curve) = S2(W(x, c)), S2(u) = 2u^2 below 0.5, 1 - 2(1 - u)^2 above;
//  - shape 6 (sharp S-curve)  = S4(W(x, c)), S4(u) = 8u^4 below 0.5, 1 - 8(1 - u)^4 above;
//  - a fade-out at x into it (0 at its start) is the fade-in curve read backwards: Fin(1 - x).
// Setting a shape in REAPER (SetMediaItemInfo_Value or its actions) also sets the curvature to the shape's preset
// (kFadePresets), so a placement writes the shape first, then the curvature.
//
// Header only, no allocation: the audio thread calls FadeCurveGain per sample.

#include <algorithm>
#include <cmath>

// The curve of one fade: REAPER's shape index and curvature.
struct FadeCurve {
    int    shape = 0;
    double dir = 0.0;
    bool operator==(const FadeCurve& o) const { return shape == o.shape && dir == o.dir; }
    bool operator!=(const FadeCurve& o) const { return !(*this == o); }
};

// REAPER's fade shape menu / actions, in its menu order: the shape and the curvature each one sets (measured), its
// name, and REAPER's action ids for the fade-in and the fade-out.
struct FadePreset {
    int         shape;
    double      dir;
    const char* name;
    const char* in_cmd;
    const char* out_cmd;
};
inline constexpr FadePreset kFadePresets[7] = {
    {0, 0.0, "Linear", "41514", "41521"},
    {1, 0.5, "Slight convex", "41515", "41522"},
    {2, -0.5, "Slight concave", "41516", "41523"},
    {3, 1.0, "Sharp convex", "41517", "41524"},
    {4, -1.0, "Sharp concave", "41518", "41525"},
    {5, 0.0, "Slight S-curve", "41519", "41526"},
    {6, 0.0, "Sharp S-curve", "41836", "41837"},
};

// A shape index REAPER knows (0..6), else 0.
inline int FadeShapeOk(int shape) { return shape >= 0 && shape <= 6 ? shape : 0; }
// The curvature REAPER sets with a shape.
inline double FadePresetDir(int shape) { return kFadePresets[FadeShapeOk(shape)].dir; }
// A curvature clamped into [-1, 1]; not finite = the shape's preset.
inline double FadeDirOr(int shape, double dir) { return std::isfinite(dir) ? std::min(std::max(dir, -1.0), 1.0) : FadePresetDir(shape); }
// The preset a curve is (its index in kFadePresets: the shape with its preset curvature), -1 when its curvature was
// changed by hand.
inline int FadePresetOf(const FadeCurve& c)
{
    const int s = FadeShapeOk(c.shape);
    return std::fabs(FadeDirOr(s, c.dir) - kFadePresets[s].dir) < 1e-9 ? s : -1;
}

namespace fade_detail {
inline double Warp(double x, double c)
{
    const double y = 1.0 - x;
    if (c >= 0.0) {
        const double q2 = 1.0 - y * y, q4 = 1.0 - y * y * y * y;
        return c <= 0.5 ? x + (q2 - x) * (2.0 * c) : q2 + (q4 - q2) * (2.0 * c - 1.0);
    }
    const double a = -c, p2 = x * x, p4 = p2 * p2;
    return a <= 0.5 ? x + (p2 - x) * (2.0 * a) : p2 + (p4 - p2) * (2.0 * a - 1.0);
}
}  // namespace fade_detail

// The fade-in gain at x in [0, 1] (0 -> 0, 1 -> 1) of `shape` at curvature `dir` (not finite: the shape's preset).
inline double FadeCurveGain(double x, int shape, double dir)
{
    x = std::isfinite(x) ? std::min(std::max(x, 0.0), 1.0) : 0.0;
    shape = FadeShapeOk(shape);
    const double u = fade_detail::Warp(x, FadeDirOr(shape, dir));
    if (shape == 5) return u < 0.5 ? 2.0 * u * u : 1.0 - 2.0 * (1.0 - u) * (1.0 - u);
    if (shape == 6) {
        if (u < 0.5) return 8.0 * u * u * u * u;
        const double v = 1.0 - u;
        return 1.0 - 8.0 * v * v * v * v;
    }
    return u;
}
inline double FadeCurveGain(double x, const FadeCurve& c) { return FadeCurveGain(x, c.shape, c.dir); }
