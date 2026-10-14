// SPDX-License-Identifier: MIT
//
// 13-59: the pure pieces of the shared busy card (busy_progress_ui.h draws it): what a long gesture
// shows (title, an optional phase, "Item i of n · event j of m"), its bar's fill, when a frame is
// due (not before ~150 ms, then about every 100 ms) and the memory of how long a gesture took last
// time (a gesture that took long shows its card at once). Host-tested
// (tests/busy_progress_test.cpp). No REAPER, no ImGui.

#pragma once

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

namespace rav {

// When a gesture's card is first shown (a quicker gesture shows nothing: no flash), how often it is
// painted again, and how long a gesture must have taken to show its card at once the next time.
constexpr double kBusyShowAfterS = 0.15;
constexpr double kBusyRepaintS = 0.1;
constexpr double kBusyLongS = 0.3;

// Where a long gesture is. `item` = the index of the item worked on (0-based) of `items` (0 = not
// counted); `step` = the steps of that item done so far, of `steps` (0 = not counted yet).
struct BusyFrame {
    std::string title;     // "Reshuffling sounds"
    std::string phase;     // "" or a short stage ("Reading items")
    std::string unit = "Item";
    int         item = 0, items = 0;
    std::string step_unit = "event";
    int         step = 0, steps = 0;
};

// The bar's fill, 0..1: the items done plus the share of the current one; without items, the steps
// done; 0 when nothing is counted.
inline float BusyFraction(const BusyFrame& f)
{
    const float within =
        f.steps > 0 ? static_cast<float>(std::clamp(f.step, 0, f.steps)) / static_cast<float>(f.steps) : 0.0f;
    if (f.items <= 0) return std::clamp(within, 0.0f, 1.0f);
    const int item = std::clamp(f.item, 0, f.items);
    return std::clamp((static_cast<float>(item) + within) / static_cast<float>(f.items), 0.0f, 1.0f);
}

// "Phase · Item 2 of 5 · event 40 of 120": each part only when it is known ("Event 40 of 120" without
// items, "Working..." when nothing is counted and there is no phase).
inline std::string BusyText(const BusyFrame& f)
{
    std::string out = f.phase;
    auto        add = [&out](const std::string& s) {
        if (s.empty()) return;
        if (!out.empty()) out += " \xC2\xB7 ";
        out += s;
    };
    char buf[96];
    if (f.items > 0) {
        const int item = std::clamp(f.item + 1, 1, f.items);
        std::snprintf(buf, sizeof(buf), "%s %d of %d", f.unit.c_str(), item, f.items);
        add(buf);
    }
    if (f.steps > 0) {
        std::string unit = f.step_unit;
        if (f.items <= 0 && !unit.empty() && unit[0] >= 'a' && unit[0] <= 'z') unit[0] = static_cast<char>(unit[0] - 32);
        std::snprintf(buf, sizeof(buf), "%s %d of %d", unit.c_str(), std::clamp(f.step, 0, f.steps), f.steps);
        add(buf);
    }
    if (out.empty()) out = "Working...";
    return out;
}

// When a frame of the card is due. Start: the gesture begins (`at_once`: its card shows from the
// first check). Due: the first frame once kBusyShowAfterS has passed since Start (or at once), then
// one at least kBusyRepaintS after the last; a clock going back is due at once.
class BusyGate {
public:
    void Start(double now_s, bool at_once = false)
    {
        start_ = now_s;
        at_once_ = at_once;
        shown_ = false;
    }
    bool Due(double now_s)
    {
        if (!shown_) {
            if (!at_once_ && now_s >= start_ && now_s - start_ < kBusyShowAfterS - 1e-9) return false;
            shown_ = true;
            last_ = now_s;
            return true;
        }
        if (now_s >= last_ && now_s - last_ < kBusyRepaintS - 1e-9) return false;
        last_ = now_s;
        return true;
    }
    bool Shown() const { return shown_; }

private:
    double start_ = 0.0, last_ = 0.0;
    bool   at_once_ = false, shown_ = false;
};

// How long each gesture (by its title) took last time, for this session.
class BusyMemory {
public:
    void Record(const std::string& title, double seconds) { last_[title] = seconds; }
    // True when its last run took kBusyLongS or more: the card shows at once.
    bool WasLong(const std::string& title) const
    {
        const auto it = last_.find(title);
        return it != last_.end() && it->second >= kBusyLongS;
    }

private:
    std::map<std::string, double> last_;
};

}  // namespace rav
