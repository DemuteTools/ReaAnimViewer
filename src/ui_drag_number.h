// The drag gesture of ui::DragNumber (ui_theme.h), without ImGui: one call per frame with that
// frame's mouse and item state. Pure, so the host tests drive it frame by frame.
#pragma once

#include <cmath>

namespace rav::ui {

// v rounded to `decimals` (0..9), never -0.
inline double DragRound(double v, int decimals)
{
    const double k = std::pow(10.0, decimals < 0 ? 0 : (decimals > 9 ? 9 : decimals));
    const double r = std::round(v * k) / k;
    return r == 0.0 ? 0.0 : r;
}

struct DragGesture {
    double start = 0.0;       // the value when the press began
    double acc = 0.0;         // the drag's change so far
    float  press_x = 0.0f;
    bool   moved = false;     // past the drag threshold: a drag, not a click
    bool   cancelled = false; // Esc during the drag: inert until the release
};

// One frame of the field's item.
struct DragFrame {
    bool  activated = false, active = false, deactivated = false;
    bool  esc = false, shift = false;
    float mouse_x = 0.0f, mouse_dx = 0.0f, threshold = 6.0f;
};

enum class DragStepEvent { None, Live, Commit, Cancel, Click };

// *value is the value the caller shows this frame. On Live and Commit it is set to the dragged
// value: the release frame commits the drag's own value, so a caller that shows the stored
// value (and does not keep the Live one) still writes what was dragged. Click = released
// without a drag (the caller then types a value).
inline DragStepEvent DragStep(DragGesture& g, const DragFrame& f, double step, int decimals, double* value)
{
    DragStepEvent ev = DragStepEvent::None;
    if (f.activated) {
        g.start = *value;
        g.acc = 0.0;
        g.press_x = f.mouse_x;
        g.moved = false;
        g.cancelled = false;
    }
    if (f.active && !g.cancelled) {
        const double k = step * (f.shift ? 0.1 : 1.0);
        if (!g.moved && std::fabs(f.mouse_x - g.press_x) >= f.threshold) {
            g.moved = true;
            g.acc = static_cast<double>(f.mouse_x - g.press_x) * k;
        } else if (g.moved) {
            g.acc += static_cast<double>(f.mouse_dx) * k;
        }
        if (f.esc) {
            g.cancelled = true;
            *value = g.start;
            ev = DragStepEvent::Cancel;
        } else if (g.moved) {
            const double nv = DragRound(g.start + g.acc, decimals);
            if (nv != *value) {
                *value = nv;
                ev = DragStepEvent::Live;
            }
        }
    }
    if (f.deactivated && !g.cancelled) {
        if (g.moved) {
            *value = DragRound(g.start + g.acc, decimals);
            ev = DragStepEvent::Commit;
        } else {
            ev = DragStepEvent::Click;
        }
    }
    return ev;
}

}  // namespace rav::ui
