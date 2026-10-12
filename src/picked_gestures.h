// SPDX-License-Identifier: MIT
//
// A gesture picked in a menu while the panel draws, run once the panel is drawn (13-8 fix, Antho
// 2026-10-12: "I pick a Switch value, it does not change; again, and REAPER stops responding").
// Pure, no REAPER, no ImGui.
//
// A menu lists what the project's sound store holds (its libraries, its Switch values) by
// reference. A sound gesture may replace that store (an auto marker's value or library is written
// to its marker type's store entry): run from inside the menu, the
// rest of the menu then reads freed memory, and the gesture itself read its value through a
// reference into the store it had just replaced. So a menu only picks: the gesture is a closure
// that holds copies of what it needs, run by Run() after the widgets.
//
// Run() runs the picks made so far, each once, in pick order. A pick made while they run waits
// for the next Run(); a Run() from inside a running pick does nothing (nothing re-enters).

#pragma once

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace rav {

class PickedGestures {
public:
    void Pick(std::function<void()> f)
    {
        if (f) picks_.push_back(std::move(f));
    }

    bool Pending() const { return !picks_.empty(); }

    // The number of picks run (0 inside a running pick).
    size_t Run()
    {
        if (running_) return 0;
        std::vector<std::function<void()>> now;
        now.swap(picks_);  // a pick made while these run waits for the next Run
        running_ = true;
        struct Done {
            bool& flag;
            ~Done() { flag = false; }
        } done{running_};
        for (const std::function<void()>& f : now) f();
        return now.size();
    }

private:
    std::vector<std::function<void()>> picks_;
    bool                               running_ = false;
};

}  // namespace rav
