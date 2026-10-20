// SPDX-License-Identifier: MIT
//
// Freeze audit #8: a join that gives up. REAPER's unload used to wait for ever on a worker stuck in a file call on a dead
// share or an offline cloud drive, so REAPER hung at quit. JoinWithin waits up to `wait_s` for the worker to report that
// it finished (`finished()`), joins it if so, and otherwise detaches it and PINS the DLL (the thread may still run RAV's
// code: the module must stay mapped for the rest of the process). Host-tested (tests/timed_join_test.cpp).

#pragma once

#include <chrono>
#include <thread>
#include <vector>

#include "watchdog.h"

namespace rav {

constexpr double kJoinWaitS = 3.0;

namespace detail {
template <class Pred>
bool WaitFor(Pred finished, double wait_s)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                              std::chrono::duration<double>(wait_s < 0.0 ? 0.0 : wait_s));
    while (!finished()) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}
}  // namespace detail

// True when the thread was joined (or there was none); false when it was abandoned.
template <class Pred>
bool JoinWithin(std::thread& th, Pred finished, double wait_s = kJoinWaitS)
{
    if (!th.joinable()) return true;
    if (detail::WaitFor(finished, wait_s)) {
        th.join();
        return true;
    }
    th.detach();
    RAV_PIN_MODULE();
    return false;
}

// The same for several threads sharing one `finished` (all of them ended).
template <class Pred>
bool JoinAllWithin(std::vector<std::thread>& ths, Pred finished, double wait_s = kJoinWaitS)
{
    bool any = false;
    for (const std::thread& t : ths) any = any || t.joinable();
    if (!any) return true;
    const bool ended = detail::WaitFor(finished, wait_s);
    for (std::thread& t : ths) {
        if (!t.joinable()) continue;
        if (ended) t.join();
        else t.detach();
    }
    if (!ended) RAV_PIN_MODULE();
    return ended;
}

}  // namespace rav
