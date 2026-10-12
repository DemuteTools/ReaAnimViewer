// SPDX-License-Identifier: MIT
//
// Host test of src/ui_drag_number.h (story 13-7b fix): a DragNumber drag commits the dragged
// value on its release frame, whether the caller keeps the Live value (the volume fader's dB
// field) or shows the stored one each frame (the Blend lanes' fields did): the Blend layer
// volume and offset no longer go back to their stored value when the drag is released.

#include "ui_drag_number.h"

#include <cstdio>

using namespace rav::ui;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

// Drags a field from x=100 by `dx` px (in 10 px steps), releases, returns the release event;
// *committed is the value the caller writes. `keep_live`: the caller shows its Live value
// next frame; otherwise it shows `stored` every frame.
static DragStepEvent Drag(double stored, float dx, bool keep_live, double step, int decimals, double* committed,
                          bool esc = false)
{
    DragGesture g;
    double      live = stored;
    float       x = 100.0f;
    DragFrame   f;
    f.activated = true;
    f.active = true;
    f.mouse_x = x;
    double v = stored;
    DragStep(g, f, step, decimals, &v);
    f.activated = false;
    const int n = static_cast<int>(dx / 10.0f);
    for (int i = 0; i < n; ++i) {
        x += 10.0f;
        f.mouse_x = x;
        f.mouse_dx = 10.0f;
        f.esc = esc && i == n - 1;
        v = keep_live ? live : stored;
        if (DragStep(g, f, step, decimals, &v) == DragStepEvent::Live) live = v;
    }
    f.esc = false;
    f.active = false;
    f.deactivated = true;
    f.mouse_dx = 0.0f;
    v = keep_live ? live : stored;
    const DragStepEvent ev = DragStep(g, f, step, decimals, &v);
    *committed = v;
    return ev;
}

static void TestStatelessCallerCommitsTheDrag()
{
    // The Blend lane dB field: shows the node's 0 dB each frame; dragged 50 px at 0.1 dB/px.
    double c = 0.0;
    CHECK(Drag(0.0, 50.0f, false, 0.1, 1, &c) == DragStepEvent::Commit);
    CHECK(c == 5.0);  // was 0.0: the release wrote the stored value back
    // The lane ms field: 120 ms dragged 40 px at 1 ms/px.
    CHECK(Drag(120.0, 40.0f, false, 1.0, 0, &c) == DragStepEvent::Commit);
    CHECK(c == 160.0);
}

static void TestBothCallersWriteTheSameValue()
{
    double a = 0.0, b = 0.0;
    CHECK(Drag(-3.0, 70.0f, true, 0.1, 1, &a) == DragStepEvent::Commit);
    CHECK(Drag(-3.0, 70.0f, false, 0.1, 1, &b) == DragStepEvent::Commit);
    CHECK(a == 4.0);
    CHECK(a == b);
}

static void TestEscAndClick()
{
    double c = 0.0;
    CHECK(Drag(2.0, 50.0f, false, 0.1, 1, &c, true) == DragStepEvent::None);  // cancelled: the release writes nothing
    CHECK(Drag(2.0, 0.0f, false, 0.1, 1, &c) == DragStepEvent::Click);         // no drag: type a value
    CHECK(c == 2.0);
}

int main()
{
    TestStatelessCallerCommitsTheDrag();
    TestBothCallersWriteTheSameValue();
    TestEscAndClick();
    if (g_fail) return 1;
    std::printf("ui_drag_number: all passed\n");
    return 0;
}
