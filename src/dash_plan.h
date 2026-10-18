#pragma once
// Repeated marks along one axis (a dashed line's dashes, a hatch's strokes) drawn only where
// they can be seen. A shape far wider than the screen (a zone at deep zoom on a long file)
// used to be walked over its whole length, ~6 px per line: millions of lines a frame, and past
// ~1.3e8 px `x += 6.0f` no longer moved x at all, an endless loop (the freeze audit, #1).
// Here the visible marks are counted in double, on the phase of the shape's own start (the
// dashes do not swim while it scrolls), and never more than a hard cap. Header only, no ImGui.
#include <algorithm>
#include <cmath>

namespace dash {

constexpr int kMaxMarks = 20000;  // far above any screen (a 8K line of 1 px dashes is ~4000)

struct Run {
    double first = 0.0;  // the first visible mark's position, start + k * step
    double step = 1.0;
    int    count = 0;
};

// Marks at start + k * step (k >= 0) while below `end`, each covering [p, p + extent]: those
// touching [lo, hi] (the draw list's clip rect on that axis). Nothing for a non-finite input.
inline Run Visible(double start, double end, double step, double extent, double lo, double hi, int cap = kMaxMarks)
{
    Run r;
    r.step = step;
    if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(step) || !std::isfinite(extent) ||
        !std::isfinite(lo) || !std::isfinite(hi))
        return r;
    if (!(step > 0.0) || !(end > start) || hi < lo || cap <= 0) return r;
    const double k0 = std::max(0.0, std::ceil((lo - std::max(0.0, extent) - start) / step));
    const double k1 = std::min(std::ceil((end - start) / step), std::floor((hi - start) / step) + 1.0);
    if (!(k1 > k0)) return r;
    r.count = static_cast<int>(std::min(static_cast<double>(cap), k1 - k0));
    r.first = start + k0 * step;
    return r;
}

// A screen coordinate kept near the visible span [lo, hi]: far enough out that nothing drawn to
// it shows a cut, near enough that float steps stay exact. NaN goes to lo.
inline float ClampCoord(double x, double lo, double hi, double margin = 4096.0)
{
    const double a = lo - margin, b = hi + margin;
    if (!(x > a)) return static_cast<float>(a);
    if (x > b) return static_cast<float>(b);
    return static_cast<float>(x);
}

}  // namespace dash
