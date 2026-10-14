// SPDX-License-Identifier: MIT
//
// Host test of the viewer's full screen plan (src/viewer_fullscreen_plan.h, story 6-5-9): the
// window styles of each mode, the monitor / work area cover, and when Esc leaves full screen.
//   c++ -std=c++17 -I src tests/viewer_fullscreen_plan_test.cpp -o t && ./t

#include "viewer_fullscreen_plan.h"

#include <cstdio>

using namespace rav::fullscreen;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

constexpr std::uint32_t kClipChildren = 0x02000000u, kClipSiblings = 0x04000000u;

}  // namespace

int main()
{
    // The viewer is created WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN; the docker shows it.
    const std::uint32_t docked = kWsChild | kClipSiblings | kClipChildren | kWsVisible;
    const std::uint32_t full = FullScreenStyle(docked);
    CHECK((full & kWsPopup) != 0);
    CHECK((full & kWsChild) == 0);
    CHECK((full & kWsVisible) == 0);  // shown by SetWindowPos once placed
    CHECK((full & kFrameBits) == 0);  // borderless
    CHECK((full & (kClipSiblings | kClipChildren)) == (kClipSiblings | kClipChildren));
    // A frame bit that crept in (a docker's) goes too.
    CHECK((FullScreenStyle(docked | kWsCaption | kWsThickFrame | kWsSysMenu) & kFrameBits) == 0);

    // Back: the child it was created as, hidden (the docker shows it), clip bits kept.
    const std::uint32_t back = DockedStyle(full | kWsVisible);
    CHECK(back == (kWsChild | kClipSiblings | kClipChildren));
    // Round trip of the created style.
    CHECK(DockedStyle(FullScreenStyle(kWsChild | kClipSiblings | kClipChildren)) ==
          (kWsChild | kClipSiblings | kClipChildren));
    // Repeated round trips are stable (enter / leave 10 times).
    std::uint32_t s = docked;
    for (int i = 0; i < 10; ++i) s = DockedStyle(FullScreenStyle(s));
    CHECK(s == (kWsChild | kClipSiblings | kClipChildren));

    // Extended style: no edges full screen, the rest kept.
    const std::uint32_t ex = kWsExClientEdge | kWsExWindowEdge | 0x00000008u /* topmost: kept as is */;
    CHECK(FullScreenExStyle(ex) == 0x00000008u);
    CHECK(FullScreenExStyle(0) == 0);

    // The cover: the whole monitor, a secondary one at negative coordinates too.
    ScreenRect m;
    m.left = 0, m.top = 0, m.right = 2560, m.bottom = 1440;
    Placement p = CoverMonitor(m);
    CHECK(p.x == 0 && p.y == 0 && p.w == 2560 && p.h == 1440);
    m.left = -1920, m.top = -200, m.right = 0, m.bottom = 880;
    p = CoverMonitor(m);
    CHECK(p.x == -1920 && p.y == -200 && p.w == 1920 && p.h == 1080);
    m.left = 10, m.top = 10, m.right = 10, m.bottom = 5;  // degenerate: at least 1 x 1
    p = CoverMonitor(m);
    CHECK(p.w == 1 && p.h == 1);

    // Story 6-5-10 -- the work area: the taskbar is never covered, wherever it is.
    {
        ScreenRect mon{0, 0, 1920, 1080};
        AutoHideEdges none;
        Placement w = CoverWorkArea(mon, ScreenRect{0, 0, 1920, 1040}, none);  // bottom taskbar
        CHECK(w.x == 0 && w.y == 0 && w.w == 1920 && w.h == 1040);
        w = CoverWorkArea(mon, ScreenRect{0, 40, 1920, 1080}, none);  // top
        CHECK(w.x == 0 && w.y == 40 && w.w == 1920 && w.h == 1040);
        w = CoverWorkArea(mon, ScreenRect{62, 0, 1920, 1080}, none);  // left
        CHECK(w.x == 62 && w.y == 0 && w.w == 1858 && w.h == 1080);
        w = CoverWorkArea(mon, ScreenRect{0, 0, 1858, 1080}, none);  // right
        CHECK(w.x == 0 && w.w == 1858 && w.h == 1080);
        // A secondary monitor at negative coordinates, its own taskbar at the bottom.
        w = CoverWorkArea(ScreenRect{-1920, -200, 0, 880}, ScreenRect{-1920, -200, 0, 840}, none);
        CHECK(w.x == -1920 && w.y == -200 && w.w == 1920 && w.h == 1040);
        // Auto-hide at the bottom: the work area is the whole monitor; 2 px stay free at the bottom.
        AutoHideEdges ah;
        ah.bottom = true;
        w = CoverWorkArea(mon, mon, ah);
        CHECK(w.x == 0 && w.y == 0 && w.w == 1920 && w.h == 1080 - kAutoHideGap);
        // Auto-hide on the left, the right, the top.
        AutoHideEdges l;
        l.left = true;
        w = CoverWorkArea(mon, mon, l);
        CHECK(w.x == kAutoHideGap && w.w == 1920 - kAutoHideGap && w.h == 1080);
        AutoHideEdges r;
        r.right = true;
        w = CoverWorkArea(mon, mon, r);
        CHECK(w.x == 0 && w.w == 1920 - kAutoHideGap);
        AutoHideEdges t;
        t.top = true;
        w = CoverWorkArea(mon, mon, t);
        CHECK(w.y == kAutoHideGap && w.h == 1080 - kAutoHideGap);
        // An auto-hide bar on an edge a docked bar already keeps free: nothing more taken.
        w = CoverWorkArea(mon, ScreenRect{0, 0, 1920, 1040}, ah);
        CHECK(w.h == 1040);
        // A bad work area (empty, or outside the monitor): the whole monitor.
        w = CoverWorkArea(mon, ScreenRect{0, 0, 0, 0}, none);
        CHECK(w.x == 0 && w.y == 0 && w.w == 1920 && w.h == 1080);
        w = CoverWorkArea(mon, ScreenRect{-5, 0, 1920, 1080}, none);
        CHECK(w.w == 1920 && w.x == 0);
    }

    // Esc: leaves only full screen, and only when nothing else takes it.
    EscState e;
    CHECK(!EscLeavesFullScreen(e));
    e.full_screen = true;
    CHECK(EscLeavesFullScreen(e));
    {
        EscState t = e;
        t.shortcuts_popup = true;
        CHECK(!EscLeavesFullScreen(t));
    }
    {
        EscState t = e;
        t.delete_confirm = true;
        CHECK(!EscLeavesFullScreen(t));
    }
    {
        EscState t = e;
        t.tagging_gesture = true;
        CHECK(!EscLeavesFullScreen(t));
    }
    {
        EscState t = e;
        t.text_input = true;
        CHECK(!EscLeavesFullScreen(t));
    }
    {
        EscState t = e;
        t.recording = true;
        CHECK(!EscLeavesFullScreen(t));
    }
    {
        EscState t = e;
        // An ImGui menu / combo open at key-down: this Esc closes it, full screen stays; the next leaves.
        t.imgui_popup = true;
        CHECK(!EscLeavesFullScreen(t));
        t.imgui_popup = false;
        CHECK(EscLeavesFullScreen(t));
    }
    {
        EscState t = e;
        t.library_takes_esc = true;
        CHECK(!EscLeavesFullScreen(t));
    }

    if (g_fails == 0) std::printf("viewer_fullscreen_plan: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
