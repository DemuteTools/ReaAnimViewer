// SPDX-License-Identifier: MIT
//
// Host test of the bone-event rule engine (src/bone_events.h, Epic 10, spike 10-0). One
// synthetic test per rule. No REAPER, no Windows: any C++17 compiler.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "bone_events.h"
#include "bone_presets.h"
#include "footstep_measure.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

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

bool Near(double a, double b, double tol = 1e-9)
{
    return std::fabs(a - b) < tol;
}

// A track sampled from y(t) (x, z from fx, fz), at `rate` over [0, dur].
BoneTrack Track(double dur, double rate, const std::function<double(double)>& fy,
                const std::function<double(double)>& fx = nullptr, const std::function<double(double)>& fz = nullptr)
{
    BoneTrack t;
    t.rate_hz = rate;
    const int n = static_cast<int>(std::floor(dur * rate + 1e-9)) + 1;
    for (int i = 0; i < n; ++i) {
        const double s = i / rate;
        t.pos.push_back(Vec3d{fx ? fx(s) : 0.0, fy(s), fz ? fz(s) : 0.0});
    }
    return t;
}

// Piecewise-linear y through (t, y) keys, held at the ends.
std::function<double(double)> Pw(std::vector<std::pair<double, double>> k)
{
    return [k](double t) {
        if (t <= k.front().first) return k.front().second;
        for (size_t i = 1; i < k.size(); ++i)
            if (t <= k[i].first) {
                const double f = (t - k[i - 1].first) / (k[i].first - k[i - 1].first);
                return k[i - 1].second + (k[i].second - k[i - 1].second) * f;
            }
        return k.back().second;
    };
}

// One block, one condition on track 0's height above floor 0.
Block HeightBlock(Direction dir, double th, double margin = 0.0)
{
    Condition c;
    c.signal.bones = {0};
    c.dir = dir;
    c.threshold = th;
    c.margin = margin;
    Block b;
    b.marker = "E";
    b.conditions = {c};
    return b;
}

}  // namespace

int main()
{
    const double kMs = 0.001;

    // ---- Threshold directions + sub-frame crossing --------------------------------------
    {
        // Below: y = 1 - t crosses 0.4987 at t = 0.5013 (between 240 Hz samples).
        std::vector<BoneTrack> tr = {Track(1.0, 240.0, [](double t) { return 1.0 - t; })};
        auto ev = Detect({HeightBlock(Direction::Below, 0.4987)}, tr);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(Near(ev[0].time_s, 0.5013, kMs));
        // Above: y = t crosses 0.3013.
        tr = {Track(1.0, 240.0, [](double t) { return t; })};
        ev = Detect({HeightBlock(Direction::Above, 0.3013)}, tr);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(Near(ev[0].time_s, 0.3013, kMs));
        // Neither fires on the other direction's crossing.
        CHECK(Detect({HeightBlock(Direction::Below, 0.3013)}, tr).empty());
    }
    {
        // Sub-frame at 30 Hz: crossing at t = 0.517 lands within 1 ms (frames are 33 ms apart).
        std::vector<BoneTrack> tr = {Track(1.0, 30.0, [](double t) { return 2.0 - 2.0 * t; })};
        auto ev = Detect({HeightBlock(Direction::Below, 2.0 - 2.0 * 0.517)}, tr);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(Near(ev[0].time_s, 0.517, kMs));
    }

    // ---- Hysteresis blocks re-fire --------------------------------------------------------
    {
        // Down under 0.5, bounce to 0.55 (< 0.5 + 0.1), down again.
        auto y = Pw({{0, 1}, {0.2, 0}, {0.6, 0}, {0.7, 0.55}, {0.8, 0}, {1.5, 0}});
        std::vector<BoneTrack> tr = {Track(1.5, 240.0, y)};
        CHECK(Detect({HeightBlock(Direction::Below, 0.5, 0.1)}, tr).size() == 1);
        CHECK(Detect({HeightBlock(Direction::Below, 0.5, 0.0)}, tr).size() == 2);  // no margin: re-fires
    }

    // ---- Re-arm needs the level AND the cooldown -----------------------------------------
    {
        // In at 0.1, out (past re-arm) at 0.3, in again at 0.4 (inside the 500 ms cooldown),
        // stays in until 1.6 (the cooldown ends while in: no event), out, in at 1.9 -> event.
        auto y = Pw({{0, 1}, {0.2, 0}, {0.3, 1}, {0.5, 0}, {1.6, 0}, {1.7, 1}, {1.8, 1}, {2.0, 0}, {2.5, 0}});
        std::vector<BoneTrack> tr = {Track(2.5, 240.0, y)};
        Block b = HeightBlock(Direction::Below, 0.5, 0.1);
        b.cooldown_ms = 500.0;
        auto ev = Detect({b}, tr);
        CHECK(ev.size() == 2);
        if (ev.size() == 2) {
            CHECK(Near(ev[0].time_s, 0.1, kMs));
            CHECK(Near(ev[1].time_s, 1.9, kMs));
        }
        b.cooldown_ms = 0.0;
        CHECK(Detect({b}, tr).size() == 3);
    }

    {
        // An entry dropped by the cooldown spends its trigger entry: the trigger comes back in
        // at 0.4 (inside the 500 ms cooldown) and stays in; the second condition flickers
        // out and in at 1.0-1.15, long after the cooldown -> still no second event.
        std::vector<BoneTrack> tr = {
            Track(3.0, 240.0, Pw({{0, 1}, {0.2, 0}, {0.25, 0}, {0.3, 1}, {0.35, 1}, {0.45, 0}, {3.0, 0}})),
            Track(3.0, 240.0, Pw({{0, 0}, {1.0, 0}, {1.05, 1}, {1.1, 1}, {1.15, 0}, {3.0, 0}}))};
        Block b = HeightBlock(Direction::Below, 0.5, 0.1);
        Condition c2 = b.conditions[0];
        c2.signal.bones = {1};
        c2.margin = 0.0;
        b.conditions.push_back(c2);
        b.cooldown_ms = 500.0;
        auto ev = Detect({b}, tr);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(Near(ev[0].time_s, 0.1, kMs));
        // Same with the edge margin dropping the first entry: no event at the flicker.
        b.cooldown_ms = 0.0;
        std::vector<BoneTrack> tr2 = {Track(3.0, 240.0, Pw({{0, 1}, {0.02, 0}, {3.0, 0}})), tr[1]};
        DetectOptions o;
        o.edge_margin_ms = 50.0;
        CHECK(Detect({b}, tr2, o).empty());
    }

    // ---- Min hold ---------------------------------------------------------------------------
    {
        // A 20 ms dip under 0.5, then a 60 ms dip.
        auto y = Pw({{0, 1}, {0.3, 1}, {0.3005, 0}, {0.32, 0}, {0.3205, 1}, {0.6, 1}, {0.6005, 0}, {0.66, 0},
                     {0.6605, 1}, {1.0, 1}});
        std::vector<BoneTrack> tr = {Track(1.0, 1000.0, y)};
        Block b = HeightBlock(Direction::Below, 0.5);
        b.min_hold_ms = 30.0;
        auto ev = Detect({b}, tr);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(Near(ev[0].time_s, 0.60025, kMs));
        b.min_hold_ms = 0.0;
        CHECK(Detect({b}, tr).size() == 2);
    }

    // ---- Signed offset -----------------------------------------------------------------------
    {
        std::vector<BoneTrack> tr = {Track(1.0, 240.0, [](double t) { return 1.0 - t; })};
        Block b = HeightBlock(Direction::Below, 0.5);
        b.offset_ms = 30.0;
        auto ev = Detect({b}, tr);
        CHECK(ev.size() == 1 && Near(ev[0].time_s, 0.53, kMs));
        b.offset_ms = -20.0;
        ev = Detect({b}, tr);
        CHECK(ev.size() == 1 && Near(ev[0].time_s, 0.48, kMs));
    }

    // ---- Combine, reference, measure, axis -----------------------------------------------
    {
        // Bone 0: y = 1 - t, x = 3, z = 4. Bone 1: y = 0.4, x = 1, z = 0.
        std::vector<BoneTrack> tr = {
            Track(1.0, 100.0, [](double t) { return 1.0 - t; }, [](double) { return 3.0; },
                  [](double) { return 4.0; }),
            Track(1.0, 100.0, [](double) { return 0.4; }, [](double) { return 1.0; })};
        SignalSpec s;
        s.bones = {0, 1};
        auto v = [&](size_t i) { return EvaluateSignal(s, tr, 0.0).at(i); };
        // i = 20 -> t = 0.2: bone 0 at y 0.8; i = 80 -> bone 0 at y 0.2.
        s.combine = Combine::Single;
        CHECK(Near(v(20), 0.8, 1e-9));
        s.combine = Combine::Average;
        CHECK(Near(v(20), 0.6, 1e-9));
        s.combine = Combine::Lowest;
        CHECK(Near(v(20), 0.4, 1e-9));
        CHECK(Near(v(80), 0.2, 1e-9));
        s.combine = Combine::Highest;
        CHECK(Near(v(20), 0.8, 1e-9));
        CHECK(Near(v(80), 0.4, 1e-9));
        // Floor level.
        s.combine = Combine::Single;
        s.floor_y = 0.1;
        CHECK(Near(v(20), 0.7, 1e-9));
        s.floor_y = 0.0;
        // Reference = bone 1 (average of one bone).
        s.bones = {0};
        s.reference = Reference::Bones;
        s.ref_bones = {1};
        CHECK(Near(v(20), 0.4, 1e-9));
        // Axes on the relative position (2, 0.4, 4) at i = 20.
        s.axis = Axis::X;
        CHECK(Near(v(20), 2.0, 1e-9));
        s.axis = Axis::Y;
        CHECK(Near(v(20), 0.4, 1e-9));
        s.axis = Axis::Z;
        CHECK(Near(v(20), 4.0, 1e-9));
        s.axis = Axis::Horizontal;
        CHECK(Near(v(20), std::sqrt(4.0 + 16.0), 1e-9));
        s.axis = Axis::Total;
        CHECK(Near(v(20), std::sqrt(4.0 + 0.16 + 16.0), 1e-9));
        // Per-bone floors: each bone above its own floor, then the lowest.
        SignalSpec f;
        f.bones = {0, 1};
        f.combine = Combine::Lowest;
        f.bone_floors = {0.0, 0.35};
        CHECK(Near(EvaluateSignal(f, tr, 0.0).at(20), 0.05, 1e-9));  // bone 1: 0.4 - 0.35
        f.bone_floors = {0.0};  // wrong size -> no signal
        CHECK(EvaluateSignal(f, tr, 0.0).empty());
        // Bad bone index -> no signal, no events.
        f.bone_floors.clear();
        f.bones = {7};
        CHECK(EvaluateSignal(f, tr, 0.0).empty());
    }
    {
        // Speed: y = -2t, x = 3t -> vertical speed 2 (magnitude) / -2 (signed), horizontal 3,
        // total sqrt(13). Acceleration of y = t^2 -> 2. Smoothing keeps a line a line.
        std::vector<BoneTrack> tr = {Track(1.0, 240.0, [](double t) { return -2.0 * t; },
                                           [](double t) { return 3.0 * t; })};
        SignalSpec s;
        s.bones = {0};
        s.measure = Measure::Speed;
        s.axis = Axis::Vertical;
        CHECK(Near(EvaluateSignal(s, tr).at(120), 2.0, 1e-6));
        s.keep_sign = true;
        CHECK(Near(EvaluateSignal(s, tr).at(120), -2.0, 1e-6));
        s.axis = Axis::Horizontal;
        CHECK(Near(EvaluateSignal(s, tr).at(120), 3.0, 1e-6));
        s.axis = Axis::Total;
        CHECK(Near(EvaluateSignal(s, tr).at(120), std::sqrt(13.0), 1e-6));
        tr = {Track(1.0, 240.0, [](double t) { return t * t; })};
        s.axis = Axis::Vertical;
        s.measure = Measure::Acceleration;
        CHECK(Near(EvaluateSignal(s, tr, 0.0).at(120), 2.0, 1e-6));
        s.measure = Measure::Speed;
        CHECK(Near(EvaluateSignal(s, tr, 0.0).at(120), 1.0, 1e-6));  // 2t at t = 0.5
    }

    // ---- Smoothing before derivatives -----------------------------------------------------
    {
        // A 0.5 m/s line plus a +1 / -1 / 0 mm jitter (period 3: a 2-sample alternation would
        // cancel in a centred difference). Smoothed (8 ms): within 5 % of the slope; raw: not.
        std::vector<BoneTrack> tr = {Track(1.0, 240.0, [](double t) {
            const int i = static_cast<int>(std::lround(t * 240.0));
            const double j[3] = {0.001, -0.001, 0.0};
            return 0.5 * t + j[i % 3];
        })};
        SignalSpec s;
        s.bones = {0};
        s.measure = Measure::Speed;
        s.keep_sign = true;
        auto worst = [&](double smooth_ms) {
            const auto v = EvaluateSignal(s, tr, smooth_ms);
            double w = 0.0;
            for (size_t i = 20; i + 20 < v.size(); ++i) w = std::max(w, std::fabs(v[i] - 0.5));
            return w;
        };
        CHECK(worst(8.0) < 0.05 * 0.5);
        CHECK(worst(0.0) > 0.05 * 0.5);
    }

    // ---- Non-finite positions: no signal, no events, Analyse leaves the blocks alone ----
    {
        std::vector<BoneTrack> tr = {Track(1.0, 240.0, [](double t) { return 1.0 - t; })};
        tr[0].pos[100].y = std::nan("");
        SignalSpec s;
        s.bones = {0};
        CHECK(EvaluateSignal(s, tr).empty());
        Block b = HeightBlock(Direction::Below, 0.5);
        CHECK(Detect({b}, tr).empty());
        auto a = Analyse({b}, tr);
        CHECK(a.size() == 1 && Near(a[0].conditions[0].threshold, 0.5));
        tr[0].pos[100].y = 0.5;
        tr[0].pos[200].x = HUGE_VAL;
        CHECK(Detect({b}, tr).empty());
    }

    // ---- AND: fires when all hold, lands at the crossing that completed it -------------
    {
        std::vector<BoneTrack> tr = {Track(1.0, 240.0, Pw({{0, 1}, {1, 0}})),   // under 0.5 at 0.5
                                     Track(1.0, 240.0, Pw({{0, 1}, {1, 0.2}}))};  // under 0.5 at 0.625
        Block b = HeightBlock(Direction::Below, 0.5);
        Condition c2 = b.conditions[0];
        c2.signal.bones = {1};
        b.conditions.push_back(c2);
        auto ev = Detect({b}, tr);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(Near(ev[0].time_s, 0.625, kMs));  // the second condition came later
        std::swap(b.conditions[0], b.conditions[1]);
        ev = Detect({b}, tr);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(Near(ev[0].time_s, 0.625, kMs));  // the trigger came later
        std::swap(b.conditions[0], b.conditions[1]);
        // A condition that never holds blocks the AND (bone 1 never goes under 0.2).
        b.conditions[1].threshold = 0.1;
        CHECK(Detect({b}, tr).empty());
    }
    {
        // Condition 1 flickering while the trigger stays in: one event (no cooldown set).
        std::vector<BoneTrack> tr = {Track(2.0, 240.0, Pw({{0, 1}, {0.5, 0}, {2.0, 0}})),
                                     Track(2.0, 240.0, Pw({{0, 0}, {0.8, 0}, {0.9, 1}, {1.0, 0}, {2.0, 0}}))};
        Block b = HeightBlock(Direction::Below, 0.5);
        Condition c2 = b.conditions[0];
        c2.signal.bones = {1};
        b.conditions.push_back(c2);
        auto ev = Detect({b}, tr);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(Near(ev[0].time_s, 0.25, kMs));
    }

    // ---- Sensitivity (strength = peak downward speed before the event) ------------------
    {
        // A fast landing (1 m in 0.1 s) and a slow one (0.2 m in 0.1 s).
        auto y = Pw({{0, 1}, {0.4, 1}, {0.5, 0}, {0.8, 0}, {0.9, 0.2}, {1.0, 0.2}, {1.1, 0}, {1.5, 0}});
        std::vector<BoneTrack> tr = {Track(1.5, 240.0, y)};
        Block b = HeightBlock(Direction::Below, 0.05, 0.02);
        b.strength_signal.bones = {0};
        b.strength_signal.measure = Measure::Speed;
        b.strength_signal.keep_sign = true;
        b.strength_sign = -1.0;
        DetectOptions o;
        auto ev = Detect({b}, tr, o);
        CHECK(ev.size() == 2);
        if (ev.size() == 2) {
            CHECK(Near(ev[0].strength, 10.0, 0.5));
            CHECK(Near(ev[1].strength, 2.0, 0.2));
            CHECK(ev[0].speed >= 0.0);
        }
        o.sensitivity = 0.5;
        ev = Detect({b}, tr, o);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(ev[0].time_s < 0.6);
    }

    // ---- Edge margin ----------------------------------------------------------------------
    {
        // Crossings at 0.01, 0.5 and 0.99.
        auto y = Pw({{0, 1}, {0.02, 0}, {0.3, 0}, {0.31, 1}, {0.49, 1}, {0.51, 0}, {0.7, 0}, {0.71, 1}, {0.98, 1},
                     {1.0, 0}});
        std::vector<BoneTrack> tr = {Track(1.0, 1000.0, y)};
        Block b = HeightBlock(Direction::Below, 0.5, 0.1);
        CHECK(Detect({b}, tr).size() == 3);
        DetectOptions o;
        o.edge_margin_ms = 50.0;
        auto ev = Detect({b}, tr, o);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(Near(ev[0].time_s, 0.5, kMs));
    }

    // ---- Analyse on a bimodal height ------------------------------------------------------
    {
        // Floor 0.03 (stance), swing 0.15, a 10 ms lift and a 100 ms landing that slows
        // down into the ground; offset by a world floor of 1.0.
        auto y = [](double t) {
            const double ph = std::fmod(t, 1.0);
            double h;
            if (ph < 0.5) h = 0.03;
            else if (ph < 0.51) h = 0.03 + 0.12 * (ph - 0.5) / 0.01;
            else if (ph < 0.9) h = 0.15;
            else h = 0.03 + 0.12 * ((1.0 - ph) / 0.1) * ((1.0 - ph) / 0.1);
            return 1.0 + h;
        };
        std::vector<BoneTrack> tr = {Track(4.0, 240.0, y)};
        Block b = HeightBlock(Direction::Below, 99.0);
        Condition sp = b.conditions[0];
        sp.signal.measure = Measure::Speed;
        b.conditions.push_back(sp);
        auto a = Analyse({b}, tr);
        CHECK(a.size() == 1 && a[0].conditions.size() == 2);
        if (a.size() == 1 && a[0].conditions.size() == 2) {
            const Condition& h = a[0].conditions[0];
            CHECK(Near(h.signal.floor_y, 1.03, 1e-3));
            CHECK(Near(h.threshold, 0.25 * 0.12, 2e-3));  // above the floor
            CHECK(Near(h.margin, h.threshold / 2.0, 2e-3));
            CHECK(a[0].conditions[1].threshold >= 0.0);
            CHECK(a[0].conditions[1].threshold < 0.5);  // still at least 30 % of the time
            CHECK(Near(a[0].conditions[1].margin, 0.5 * a[0].conditions[1].threshold, 1e-12));
            // Detect with the proposal: one event per landing (t = 1, 2, 3, 4 -> 3 inside).
            auto ev = Detect(a, tr);
            CHECK(ev.size() == 3);
            for (const Event& e : ev) CHECK(std::fabs(e.time_s - std::round(e.time_s)) < 0.016);
        }
        // Per-bone floors.
        AnalyseOptions o;
        o.per_bone_floor = true;
        a = Analyse({b}, tr, o);
        CHECK(a[0].conditions[0].signal.bone_floors.size() == 1);
        CHECK(Near(a[0].conditions[0].signal.bone_floors[0] + a[0].conditions[0].signal.floor_y, 1.03, 1e-3));
    }

    // ---- Angle signals: joint flexion and unwrapped yaw ----------------------------------
    {
        // Flexion at b: a straight up, c swinging away from straight down by phi = 30 t deg.
        const double d2r = 3.14159265358979323846 / 180.0;
        auto c_of = [d2r](double t) { return Vec3d{std::sin(30.0 * t * d2r), -std::cos(30.0 * t * d2r), 0.0}; };
        std::vector<BoneTrack> tr = {Track(2.0, 240.0, [](double) { return 1.0; }),
                                     Track(2.0, 240.0, [](double) { return 0.0; }),
                                     Track(2.0, 240.0, [&](double t) { return c_of(t).y; },
                                           [&](double t) { return c_of(t).x; })};
        SignalSpec s;
        s.quantity = Quantity::JointAngle;
        s.bones = {0, 1, 2};
        CHECK(Near(EvaluateSignal(s, tr).at(240), 30.0, 1e-6));  // t = 1 s
        CHECK(Near(EvaluateSignal(s, tr).at(0), 0.0, 1e-6));     // straight
        s.measure = Measure::Speed;
        s.keep_sign = true;
        CHECK(Near(EvaluateSignal(s, tr).at(240), 30.0, 1e-3));
        s.bones = {0, 1};  // a joint angle needs three bones
        CHECK(EvaluateSignal(s, tr).empty());
        // Scale-free: the same pose ten times bigger gives the same angle.
        std::vector<BoneTrack> big = tr;
        for (BoneTrack& b : big)
            for (Vec3d& q : b.pos) q = Vec3d{q.x * 10, q.y * 10, q.z * 10};
        s.bones = {0, 1, 2};
        s.measure = Measure::Position;
        CHECK(Near(EvaluateSignal(s, big).at(120), EvaluateSignal(s, tr).at(120), 1e-9));

        // Yaw of a -> b: heading psi = 150 + 200 t deg (atan2 on X/Z), across +-180.
        auto yaw_tracks = [d2r](double rate_dps) {
            auto bx = [=](double t) { return std::sin((150.0 + rate_dps * t) * d2r); };
            auto bz = [=](double t) { return std::cos((150.0 + rate_dps * t) * d2r); };
            return std::vector<BoneTrack>{Track(2.0, 240.0, [](double) { return 0.0; }),
                                          Track(2.0, 240.0, [](double) { return 0.0; }, bx, bz)};
        };
        SignalSpec y;
        y.quantity = Quantity::Yaw;
        y.bones = {0, 1};
        auto yt = yaw_tracks(200.0);
        auto v = EvaluateSignal(y, yt);
        CHECK(Near(v.at(0), 150.0, 1e-6));
        CHECK(Near(v.at(120), 250.0, 1e-6));  // past 180: unwrapped, no jump to -110
        CHECK(Near(v.at(480), 550.0, 1e-6));
        y.measure = Measure::Speed;
        v = EvaluateSignal(y, yt);
        CHECK(Near(v.at(60), 200.0, 1e-3));  // 0.25 s: the wrap is at 0.15 s
        CHECK(Near(v.at(36), 200.0, 1e-3));  // right at the wrap
        yt = yaw_tracks(-200.0);
        CHECK(Near(EvaluateSignal(y, yt).at(240), 200.0, 1e-3));  // magnitude
        y.keep_sign = true;
        CHECK(Near(EvaluateSignal(y, yt).at(240), -200.0, 1e-3));
        // Analyse: a signed angle speed "above" -> + onset_fraction * p95; a fixed limit stays.
        Block b;
        Condition c;
        c.signal = y;
        c.dir = Direction::Above;
        b.conditions = {c};
        c.auto_threshold = false;
        c.threshold = 800.0;
        b.conditions.push_back(c);
        auto a = Analyse({b}, yaw_tracks(200.0));
        CHECK(Near(a[0].conditions[0].threshold, 0.1 * 200.0, 1e-3));
        CHECK(Near(a[0].conditions[0].margin, 0.5 * 0.1 * 200.0, 1e-3));
        CHECK(a[0].conditions[1].threshold == 800.0);
    }

    // ---- Landing on a peak (sub-sample, parabolic) ---------------------------------------
    {
        // y = 1 - (t - 0.5137)^2 * 40: above 0.5 from about 0.40 to 0.63, peak at 0.5137.
        std::vector<BoneTrack> tr = {
            Track(1.0, 240.0, [](double t) { return 1.0 - 40.0 * (t - 0.5137) * (t - 0.5137); })};
        Block b = HeightBlock(Direction::Above, 0.5);
        auto ev = Detect({b}, tr);
        CHECK(ev.size() == 1);
        const double crossing = ev.empty() ? 0.0 : ev[0].time_s;
        CHECK(crossing < 0.45);  // the default lands at the crossing
        b.landing = Landing::PeakOf;
        b.peak_condition = 0;
        b.peak_max = true;
        ev = Detect({b}, tr);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) {
            CHECK(Near(ev[0].time_s, 0.5137, kMs));
            CHECK(Near(ev[0].cond_entry_s.at(0), crossing, 1e-12));  // entries stay the crossings
        }
        b.offset_ms = 10.0;
        ev = Detect({b}, tr);
        CHECK(ev.size() == 1 && Near(ev[0].time_s, 0.5237, kMs));
        // Min: the trough of a "below" span.
        std::vector<BoneTrack> tr2 = {
            Track(1.0, 240.0, [](double t) { return 40.0 * (t - 0.3021) * (t - 0.3021); })};
        Block lo = HeightBlock(Direction::Below, 0.5);
        lo.landing = Landing::PeakOf;
        lo.peak_max = false;
        ev = Detect({lo}, tr2);
        CHECK(ev.size() == 1 && Near(ev[0].time_s, 0.3021, kMs));
        // The cooldown keeps the first event; the edge margin applies to the landing.
        std::vector<BoneTrack> tr3 = {Track(1.0, 240.0, [](double t) {
            const double a = 1.0 - 400.0 * (t - 0.3) * (t - 0.3), c = 1.0 - 400.0 * (t - 0.45) * (t - 0.45);
            return std::max(a, c);
        })};
        Block two = HeightBlock(Direction::Above, 0.5, 0.2);
        two.landing = Landing::PeakOf;
        CHECK(Detect({two}, tr3).size() == 2);
        two.cooldown_ms = 200.0;
        ev = Detect({two}, tr3);
        CHECK(ev.size() == 1 && Near(ev[0].time_s, 0.3, kMs));
        DetectOptions o;
        o.edge_margin_ms = 520.0;  // a crossing at ~0.4 would pass; its landing at 0.5137 does not
        CHECK(Detect({b}, tr, o).empty());
    }

    // ---- The shared measure (footstep_measure.h) on a synthetic gait -----------------------
    {
        // Mixamo legs, in place. Left foot lands at 0.5 + k, right at 1.0 + k (heel first, toe
        // 60 ms later). The knee bends after contact: its flexion speed peaks kLag after the
        // contact. The event must land at that peak.
        const double kLag = 0.04;
        auto foot_h = [](double land) {
            return [land](double t) {
                const double ph = std::fmod(t - land + 10.0, 1.0);
                if (ph < 0.55) return 0.0;
                if (ph < 0.75) return 0.15 * (ph - 0.55) / 0.2;
                const double u = (1.0 - ph) / 0.25;
                return 0.15 * u * u;
            };
        };
        // The knee's forward push (m): a bend centred kLag after contact (raised-cosine speed,
        // 100 ms), straightening in late stance, and a swing bend while the foot is up.
        auto push = [kLag](double land) {
            return [land, kLag](double t) {
                const double ph = std::fmod(t - land + 10.05, 1.0) - 0.05;  // -0.05 .. 0.95, 0 at contact
                const double pi = 3.14159265358979323846;
                auto step = [pi](double x) { return x <= 0 ? 0.0 : x >= 1 ? 1.0 : 0.5 - 0.5 * std::cos(pi * x); };
                double k = 0.02 + 0.06 * step((ph - (kLag - 0.05)) / 0.1);  // weight acceptance bend
                k -= 0.06 * step((ph - 0.3) / 0.25);                         // straighten in stance
                k += 0.10 * step((ph - 0.72) / 0.1) - 0.10 * step((ph - 0.82) / 0.1);  // swing (foot up)
                return k;
            };
        };
        struct Leg {
            std::function<double(double)> h, k;
            double x;
        };
        auto leg_tracks = [&](const Leg& L, std::vector<BoneTrack>& out) {
            auto hip = Track(4.2, 240.0, [](double) { return 0.95; }, [&](double) { return L.x; });
            auto knee = Track(4.2, 240.0, [&](double t) { return 0.5 * (0.95 + 0.08 + L.h(t)); },
                              [&](double) { return L.x; }, [&](double t) { return L.k(t); });
            auto ankle = Track(4.2, 240.0, [&](double t) { return 0.08 + L.h(t); }, [&](double) { return L.x; });
            auto toe = Track(4.2, 240.0, [&](double t) { return 0.03 + L.h(std::max(0.0, t - 0.06)); },
                             [&](double) { return L.x; }, [](double) { return 0.15; });
            out.push_back(hip);
            out.push_back(knee);
            out.push_back(ankle);
            out.push_back(toe);
        };
        const std::vector<std::string> names = {"mixamorig:LeftUpLeg",  "mixamorig:LeftLeg",  "mixamorig:LeftFoot",
                                                "mixamorig:LeftToeBase", "mixamorig:RightUpLeg", "mixamorig:RightLeg",
                                                "mixamorig:RightFoot",  "mixamorig:RightToeBase"};
        std::vector<BoneTrack> all;
        Leg left{foot_h(0.5), push(0.5), 0.1}, right{foot_h(0.0), push(0.0), -0.1};
        leg_tracks(left, all);
        leg_tracks(right, all);
        TrackSampler sampler = [&](const std::vector<int>& idx) {
            std::vector<BoneTrack> t;
            for (int i : idx) t.push_back(all.at(static_cast<size_t>(i)));
            return t;
        };
        // The truth: the knee flexion speed peak near each contact, from the exact geometry.
        auto flex = [&](const Leg& L, double t) {
            const double ky = 0.5 * (0.95 + 0.08 + L.h(t)), kz = L.k(t), ay = 0.08 + L.h(t);
            const double ux = 0.95 - ky, uz = -kz, wx = ay - ky, wz = -kz;
            const double c = (ux * wx + uz * wz) / (std::sqrt(ux * ux + uz * uz) * std::sqrt(wx * wx + wz * wz));
            return 180.0 - std::acos(c) * 180.0 / 3.14159265358979323846;
        };
        std::vector<double> truth;
        for (double c : {0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0}) {
            const Leg& L = (std::fmod(c, 1.0) > 0.25) ? left : right;
            double best = c, best_v = -1e9;
            for (double t = c - 0.05; t <= c + 0.15; t += 0.0001) {
                const double v = (flex(L, t + 0.00005) - flex(L, t - 0.00005)) / 0.0001;
                if (v > best_v) {
                    best_v = v;
                    best = t;
                }
            }
            truth.push_back(best);
            CHECK(std::fabs(best - (c + kLag)) < 0.010);
        }
        FootstepsParams prm;
        ItemMeasure m = MeasureFootsteps(names, sampler, truth, 0.1, 4.2, prm);
        CHECK(m.skipped.empty());
        CHECK(m.match.recall == 1.0);
        CHECK(m.match.precision == 1.0);
        CHECK(m.match.max_abs_err_s <= 0.003);
        CHECK(!ItemReport("gait", m, truth, "").empty());
        CHECK(ItemJson("gait", m).find("\"n_match\":8") != std::string::npos);
        CHECK(TotalReport({m.match}, prm).find("-> GO") != std::string::npos);
        // The yaw limit gates: a limit below the (zero) yaw speed of a planted foot -> nothing.
        prm.yaw_limit_dps = -1.0;
        CHECK(MeasureFootsteps(names, sampler, truth, 0.1, 4.2, prm).det.empty());
        // A skeleton without knees is skipped with the roles named.
        std::vector<std::string> no_knee = names;
        no_knee[1] = "LeftShin";
        no_knee[5] = "RightShin";
        m = MeasureFootsteps(no_knee, sampler, truth, 0.1, 4.2, FootstepsParams{});
        CHECK(m.skipped == "roles not found: left knee, right knee");
    }

    // ---- MatchEvents ------------------------------------------------------------------------
    {
        // Nearest pair first, one-to-one: det 1.01 belongs to ref 1.0, det 1.04 is left over.
        MatchResult m = MatchEvents({1.0, 2.0}, {1.04, 1.01, 2.005}, 0.15, 0.0, 10.0);
        CHECK(m.n_ref == 2 && m.n_det == 3 && m.n_match == 2);
        CHECK(Near(m.recall, 1.0) && Near(m.precision, 2.0 / 3.0));
        CHECK(Near(m.max_abs_err_s, 0.01, 1e-9));
        CHECK(Near(m.mean_abs_err_s, 0.0075, 1e-9));
        CHECK(Near(m.mean_signed_err_s, 0.0075, 1e-9));
        CHECK(m.unmatched_det.size() == 1 && Near(m.unmatched_det[0], 1.04));
        // Outside the window: no match. The window edge counts.
        m = MatchEvents({1.0}, {1.2}, 0.15, 0.0, 10.0);
        CHECK(m.n_match == 0 && m.unmatched_ref.size() == 1 && m.unmatched_det.size() == 1);
        m = MatchEvents({1.0}, {1.15}, 0.15, 0.0, 10.0);
        CHECK(m.n_match == 1);
        // Outside [lo, hi] (a trimmed item): not counted on either side.
        m = MatchEvents({0.5, 1.0, 3.0}, {0.51, 1.0, 2.99}, 0.15, 0.8, 2.5);
        CHECK(m.n_ref == 1 && m.n_det == 1 && m.n_match == 1);
        // Empty sides.
        m = MatchEvents({}, {1.0}, 0.15, 0.0, 10.0);
        CHECK(m.n_ref == 0 && m.n_det == 1 && Near(m.recall, 1.0) && Near(m.precision, 0.0));
        m = MatchEvents({1.0}, {}, 0.15, 0.0, 10.0);
        CHECK(Near(m.recall, 0.0) && Near(m.precision, 1.0));
        // A greedy-by-order match would pair ref 1.0 with 1.08; nearest-first keeps it for 1.1.
        m = MatchEvents({1.0, 1.1}, {1.08}, 0.15, 0.0, 10.0);
        CHECK(m.n_match == 1 && m.unmatched_ref.size() == 1 && Near(m.unmatched_ref[0], 1.0));
        // Totals and the gate.
        MatchResult a = MatchEvents({1, 2, 3}, {1.01, 2.0, 3.0}, 0.15, 0, 10);
        MatchResult b = MatchEvents({1}, {1.02}, 0.15, 0, 10);
        MatchResult t = SumMatches({a, b});
        CHECK(t.n_ref == 4 && t.n_match == 4 && Near(t.max_abs_err_s, 0.02, 1e-9));
        CHECK(Near(t.mean_abs_err_s, 0.03 / 4.0, 1e-9));
        CHECK(PassesAccuracyGate(a));
        CHECK(!PassesAccuracyGate(t));  // one step 20 ms off fails it
        CHECK(!PassesAccuracyGate(MatchEvents({}, {}, 0.15, 0, 10)));  // nothing measured is no GO
        std::vector<double> ref50, det49;
        for (int i = 0; i < 50; ++i) ref50.push_back(i);
        det49.assign(ref50.begin(), ref50.end() - 1);
        CHECK(PassesAccuracyGate(MatchEvents(ref50, det49, 0.15, 0, 100)));     // 98 %
        det49.pop_back();
        CHECK(!PassesAccuracyGate(MatchEvents(ref50, det49, 0.15, 0, 100)));    // 96 %
    }

    // ---- Bad input never throws ------------------------------------------------------------
    {
        CHECK(Detect({HeightBlock(Direction::Below, 0.5)}, {}).empty());
        std::vector<BoneTrack> tr = {Track(1.0, 240.0, [](double) { return 0.0; }),
                                     Track(0.5, 240.0, [](double) { return 0.0; })};
        CHECK(Detect({HeightBlock(Direction::Below, 0.5)}, tr).empty());  // lengths differ
        Block empty;
        CHECK(Detect({empty}, {Track(1.0, 240.0, [](double) { return 0.0; })}).empty());
    }

    if (g_fails == 0) std::printf("bone_events: all tests passed\n");
    return g_fails == 0 ? 0 : 1;
}
