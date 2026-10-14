// SPDX-License-Identifier: MIT
//
// Host test of src/live_draft_phase.h (13-53): a dragged value released stays drawn until the
// store holds it, never the grab-time value for a frame.
//   c++ -std=c++17 -I src tests/live_draft_phase_test.cpp -o t && ./t

#include "live_draft_phase.h"

#include <cstdio>
#include <string>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

// A panel frame: the store, the draft, what is drawn (the rule Shown() applies).
struct Panel {
    LiveDraftPhase ph;
    double         store = 1.0, snap = 0.0, draft = 0.0;
    double Drawn(const std::string& uid) const { return ph.Shows(uid) ? draft : store; }
    void Edit(const std::string& uid, double v)
    {
        if (ph.Touch(uid)) snap = store;
        draft = snap;
        draft = v;
    }
};

}  // namespace

int main()
{
    // The drag: grabbed at 1.0, dragged to 3.0, released. Each frame: the top-of-frame rule, the
    // waveform drawn, its event, then the picked writes run.
    {
        Panel    p;
        unsigned pending = 0;
        double   pending_v = 0.0;
        auto run_picks = [&] {
            if (!pending) return;
            p.store = pending_v;  // WriteNow
            p.ph.Written(pending);
            pending = 0;
        };
        // Frame 1: button down, dragged to 2.0.
        if (p.ph.EndsAtFrameStart(true, false, false, false)) p.ph.Cancel();
        CHECK(p.Drawn("a") == 1.0);
        p.Edit("a", 2.0);
        run_picks();
        // Frame 2: still down, dragged to 3.0.
        if (p.ph.EndsAtFrameStart(true, false, false, false)) p.ph.Cancel();
        CHECK(p.Drawn("a") == 2.0);
        p.Edit("a", 3.0);
        run_picks();
        // Frame 3: released. The draft is still drawn (not the grab-time 1.0), the event commits.
        if (p.ph.EndsAtFrameStart(false, true, false, false)) p.ph.Cancel();
        CHECK(p.Drawn("a") == 3.0);
        p.Edit("a", 3.0);
        pending_v = p.draft;
        pending = p.ph.Commit("a");
        CHECK(pending != 0);
        CHECK(p.Drawn("a") == 3.0);    // the rest of the frame draws the draft too
        CHECK(p.ph.Commit("a") == 0);  // committed once
        run_picks();
        CHECK(!p.ph.Live());
        // Frame 4: the store holds it.
        if (p.ph.EndsAtFrameStart(false, false, false, false)) p.ph.Cancel();
        CHECK(p.Drawn("a") == 3.0);
        CHECK(p.store == 3.0);
    }
    // A release with no "done" (the drag ended elsewhere): kept that frame, ended the next.
    {
        Panel p;
        p.Edit("a", 5.0);
        CHECK(!p.ph.EndsAtFrameStart(false, true, false, false));
        CHECK(p.ph.EndsAtFrameStart(false, false, false, false));
        p.ph.Cancel();
        CHECK(p.Drawn("a") == 1.0);
    }
    // Held by a number field or a fader typed into.
    {
        LiveDraftPhase ph;
        ph.Touch("a");
        CHECK(!ph.EndsAtFrameStart(false, false, true, false));
        CHECK(!ph.EndsAtFrameStart(false, false, false, true));
        CHECK(!LiveDraftPhase().EndsAtFrameStart(false, false, false, false));  // nothing live: nothing to end
    }
    // A commit waiting for its write is never ended at a frame start; a stale token ends nothing.
    {
        LiveDraftPhase ph;
        ph.Touch("a");
        const unsigned t = ph.Commit("a");
        CHECK(ph.Committing());
        CHECK(!ph.EndsAtFrameStart(false, false, false, false));
        ph.Written(t + 1);
        CHECK(ph.Live());
        ph.Written(t);
        CHECK(!ph.Live());
        ph.Written(t);  // twice: nothing
        CHECK(!ph.Live());
    }
    // An edit after the commit in the same frame keeps the draft: the old write no longer ends it.
    {
        LiveDraftPhase ph;
        ph.Touch("a");
        const unsigned t = ph.Commit("a");
        CHECK(!ph.Touch("a"));
        CHECK(!ph.Committing());
        ph.Written(t);
        CHECK(ph.Shows("a"));
    }
    // Cancel ends it at once, a commit included; another Sound's draft starts over.
    {
        LiveDraftPhase ph;
        CHECK(ph.Touch("a"));
        const unsigned t = ph.Commit("a");
        ph.Cancel();
        CHECK(!ph.Shows("a"));
        ph.Written(t);
        CHECK(!ph.Live());
        CHECK(ph.Touch("a"));
        CHECK(!ph.Touch("a"));
        CHECK(ph.Touch("b"));
        CHECK(!ph.Shows("a") && ph.Shows("b"));
        CHECK(ph.Commit("a") == 0);
    }
    if (g_fails) {
        std::printf("%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("live_draft_phase: all passed\n");
    return 0;
}
