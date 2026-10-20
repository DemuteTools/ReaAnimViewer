// SPDX-License-Identifier: MIT
//
// Freeze audit #3: "retire, don't destroy". A replaced object (the project's sound store after a write or a load) is
// not freed at once: a REAPER timer can replace it while a frame or gesture further up the stack (a modal file
// dialog, a nested Undo) still holds a reference into it, and a loop over freed vectors has hung REAPER before
// (4e2e6ae, 12b48b2). The old object waits here and is freed only at a top-level point (the start of a frame or a
// timer tick that is not nested inside any RAV scope), so every stale reference stays valid memory until the stack
// unwinds. Pure, host-tested (tests/store_graveyard_test.cpp).

#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace rav {

template <class T>
class Graveyard {
public:
    void Retire(std::unique_ptr<T> p)
    {
        if (p) dead_.push_back(std::move(p));
    }
    // Frees everything retired when `top_level` (nothing of RAV is running further up the stack); returns how many.
    size_t Sweep(bool top_level)
    {
        if (!top_level) return 0;
        const size_t n = dead_.size();
        dead_.clear();
        return n;
    }
    size_t Size() const { return dead_.size(); }

private:
    std::vector<std::unique_ptr<T>> dead_;
};

}  // namespace rav
