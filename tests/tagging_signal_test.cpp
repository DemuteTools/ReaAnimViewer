// SPDX-License-Identifier: MIT
//
// Host test of the Tagging view's units, menus and clip mapping (src/tagging_signal.h,
// story 10-3). No REAPER, no Windows: any C++17 compiler.

#include "tagging_signal.h"

#include <cmath>
#include <cstdio>
#include <string>
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

SignalSpec Spec(Quantity q, Measure m)
{
    SignalSpec s;
    s.quantity = q;
    s.measure = m;
    return s;
}

}  // namespace

int main()
{
    // ---- Units and conversions ----------------------------------------------------------
    {
        const SignalSpec pos = Spec(Quantity::Point, Measure::Position);
        const SignalSpec spd = Spec(Quantity::Point, Measure::Speed);
        const SignalSpec acc = Spec(Quantity::Point, Measure::Acceleration);
        const SignalSpec ang = Spec(Quantity::JointAngle, Measure::Position);
        const SignalSpec aspd = Spec(Quantity::Yaw, Measure::Speed);
        const SignalSpec aacc = Spec(Quantity::JointAngle, Measure::Acceleration);
        CHECK(UnitOf(pos) == SignalUnit::Centimetre);
        CHECK(UnitOf(spd) == SignalUnit::MetrePerSecond);
        CHECK(UnitOf(acc) == SignalUnit::MetrePerSecond2);
        CHECK(UnitOf(ang) == SignalUnit::Degree);
        CHECK(UnitOf(aspd) == SignalUnit::DegreePerSecond);
        CHECK(UnitOf(aacc) == SignalUnit::DegreePerSecond2);
        CHECK(std::string(UnitLabel(SignalUnit::Centimetre)) == "cm");
        CHECK(std::string(UnitLabel(SignalUnit::MetrePerSecond2)) == "m/s\xC2\xB2");
        CHECK(std::string(UnitLabel(SignalUnit::DegreePerSecond)) == "\xC2\xB0/s");

        // Position: metres <-> centimetres; everything else 1:1.
        CHECK(Near(ToDisplay(pos, 0.05), 5.0));
        CHECK(Near(FromDisplay(pos, 5.0), 0.05));
        CHECK(Near(ToDisplay(spd, 1.25), 1.25) && Near(FromDisplay(spd, 1.25), 1.25));
        CHECK(Near(ToDisplay(acc, -3.0), -3.0));
        CHECK(Near(ToDisplay(ang, 42.0), 42.0) && Near(FromDisplay(aspd, 300.0), 300.0));
        for (double v : {-1.5, 0.0, 0.0123, 7.0})
            for (const SignalSpec* s : {&pos, &spd, &acc, &ang, &aspd, &aacc})
                CHECK(Near(FromDisplay(*s, ToDisplay(*s, v)), v, 1e-12));

        CHECK(FormatDisplay(pos, 0.05) == "5.0 cm");
        CHECK(FormatDisplay(spd, 0.5) == "0.50 m/s");
        CHECK(FormatDisplay(aspd, 312.4) == "312 \xC2\xB0/s");
        CHECK(FormatDisplay(ang, 12.0) == "12.0\xC2\xB0");
        CHECK(FormatDisplayNumber(pos, -0.0001) == "0.0");  // never "-0.0"
        CHECK(FormatDisplayNumber(pos, -0.02) == "-2.0");
        CHECK(UnitDragStep(SignalUnit::Centimetre) > 0.0 && UnitDragStep(SignalUnit::DegreePerSecond) > 0.0);
    }

    // ---- Menus -----------------------------------------------------------------------------
    {
        CHECK(std::string(MeasureLabel(Measure::Acceleration)) == "Acceleration");
        CHECK(std::string(AxisLabel(Axis::Horizontal)) == "horizontal");
        CHECK(std::string(DirectionLabel(Direction::Above)) == "above");
        CHECK(IsAngleSignal(Spec(Quantity::Yaw, Measure::Position)));
        CHECK(!IsAngleSignal(Spec(Quantity::Point, Measure::Position)));

        // 10-4 follow-up: joint angle and rotation are in degrees.
        CHECK(IsAngleSignal(Spec(Quantity::InteriorAngle, Measure::Position)));
        CHECK(IsAngleSignal(Spec(Quantity::Rotation, Measure::Speed)));
        CHECK(UnitOf(Spec(Quantity::InteriorAngle, Measure::Position)) == SignalUnit::Degree);
        CHECK(UnitOf(Spec(Quantity::InteriorAngle, Measure::Acceleration)) == SignalUnit::DegreePerSecond2);
        CHECK(UnitOf(Spec(Quantity::Rotation, Measure::Speed)) == SignalUnit::DegreePerSecond);
        CHECK(UnitOf(Spec(Quantity::Rotation, Measure::Position)) == SignalUnit::Degree);
        CHECK(FormatDisplay(Spec(Quantity::Rotation, Measure::Speed), 300.0) == "300 \xC2\xB0/s");

        // The kind chip: Bone / Joint angle / Rotation; older flexion and yaw show as Bend / Turn.
        CHECK(ConditionKindOf(Spec(Quantity::Point, Measure::Position)) == ConditionKind::Bone);
        CHECK(ConditionKindOf(Spec(Quantity::InteriorAngle, Measure::Position)) == ConditionKind::JointAngle);
        CHECK(ConditionKindOf(Spec(Quantity::Rotation, Measure::Position)) == ConditionKind::Rotation);
        CHECK(ConditionKindOf(Spec(Quantity::JointAngle, Measure::Position)) == ConditionKind::Bend);
        CHECK(ConditionKindOf(Spec(Quantity::Yaw, Measure::Position)) == ConditionKind::Turn);
        CHECK(std::string(ConditionKindLabel(ConditionKind::JointAngle)) == "Joint angle");
        CHECK(std::string(MeasureLabelFor(Spec(Quantity::InteriorAngle, Measure::Position), Measure::Position)) ==
              "angle");
        CHECK(std::string(MeasureLabelFor(Spec(Quantity::InteriorAngle, Measure::Speed), Measure::Speed)) ==
              "angle speed");
        CHECK(std::string(MeasureLabelFor(Spec(Quantity::Point, Measure::Position), Measure::Position)) == "Position");
        {
            Condition c = DefaultCondition();
            c.signal.bones = {4};  // the left knee role
            c.threshold = 0.07;
            SetConditionKind(c, ConditionKind::JointAngle);  // the bone stays, the rest resets
            CHECK(c.signal.quantity == Quantity::InteriorAngle && c.signal.bones == std::vector<int>{4});
            CHECK(c.signal.measure == Measure::Position && c.dir == Direction::Below && Near(c.threshold, 90.0));
            SetConditionKind(c, ConditionKind::Rotation);
            CHECK(c.signal.quantity == Quantity::Rotation && c.signal.bones == std::vector<int>{4});
            CHECK(c.signal.reference == Reference::Parent && c.signal.measure == Measure::Speed &&
                  c.signal.axis == Axis::Total && c.dir == Direction::Above && Near(c.threshold, 300.0));
            SetConditionKind(c, ConditionKind::Bone);
            CHECK(c.signal.quantity == Quantity::Point && c.signal.bones == std::vector<int>{4} &&
                  c.signal.reference == Reference::Floor && Near(c.threshold, DefaultCondition().threshold));
            Condition bend;
            bend.signal.quantity = Quantity::JointAngle;
            bend.signal.bones = {6, 4, 0};
            SetConditionKind(bend, ConditionKind::JointAngle);  // a bend keeps its middle bone
            CHECK(bend.signal.quantity == Quantity::InteriorAngle && bend.signal.bones == std::vector<int>{4});
            Condition empty;
            empty.signal.bones.clear();
            SetConditionKind(empty, ConditionKind::Rotation);  // no bone: the default one
            CHECK(empty.signal.bones.size() == 1);
        }
        // A rotation's axis: total only for speed / acceleration; an old angle on total reads as X.
        CHECK(!RotationAxisOffered(Measure::Position, Axis::Total) && RotationAxisOffered(Measure::Speed, Axis::Total));
        CHECK(RotationAxisOffered(Measure::Position, Axis::Z) && !RotationAxisOffered(Measure::Speed, Axis::Vertical));
        {
            SignalSpec r = Spec(Quantity::Rotation, Measure::Position);
            r.axis = Axis::Total;
            CHECK(RotationAxisOf(r) == Axis::X);
            r.measure = Measure::Speed;
            CHECK(RotationAxisOf(r) == Axis::Total);
            r.axis = Axis::Vertical;
            CHECK(RotationAxisOf(r) == Axis::Y);
        }
        // Spike 10-7a: Stillness (cm, deg on an angle) and Relative drop (%) are offered.
        {
            bool has_still = false, has_drop = false;
            for (Measure m : kMeasureChoices) {
                has_still = has_still || m == Measure::Stillness;
                has_drop = has_drop || m == Measure::RelativeDrop;
            }
            CHECK(has_still && has_drop);
            CHECK(std::string(MeasureLabel(Measure::Stillness)) == "Stillness" &&
                  std::string(MeasureLabel(Measure::RelativeDrop)) == "Relative drop");
            CHECK(!MeasureHint(Measure::Stillness).empty() && !MeasureHint(Measure::RelativeDrop).empty() &&
                  MeasureHint(Measure::Speed).empty());
            CHECK(MeasureHint(Measure::Stillness).find("150 ms") != std::string::npos &&
                  MeasureHint(Measure::RelativeDrop).find("300 ms") != std::string::npos);
            const SignalSpec still = Spec(Quantity::Point, Measure::Stillness);
            const SignalSpec drop = Spec(Quantity::Point, Measure::RelativeDrop);
            const SignalSpec astill = Spec(Quantity::InteriorAngle, Measure::Stillness);
            const SignalSpec adrop = Spec(Quantity::Rotation, Measure::RelativeDrop);
            CHECK(UnitOf(still) == SignalUnit::Centimetre && UnitOf(astill) == SignalUnit::Degree);
            CHECK(UnitOf(drop) == SignalUnit::Percent && UnitOf(adrop) == SignalUnit::Percent);
            CHECK(std::string(UnitLabel(SignalUnit::Percent)) == "%");
            CHECK(Near(ToDisplay(drop, 0.2), 20.0) && Near(FromDisplay(drop, 20.0), 0.2));
            CHECK(Near(ToDisplay(still, 0.012), 1.2) && Near(FromDisplay(astill, 4.0), 4.0));
            CHECK(FormatDisplay(drop, 0.2) == "20 %" && FormatDisplay(still, 0.012) == "1.2 cm" &&
                  FormatDisplay(astill, 6.75) == "6.8\xC2\xB0");
            CHECK(UnitDecimals(SignalUnit::Percent) == 0 && UnitDragStep(SignalUnit::Percent) > 0.0);
            CHECK(std::string(MeasureLabelFor(astill, Measure::Stillness)) == "angle stillness" &&
                  std::string(MeasureLabelFor(astill, Measure::RelativeDrop)) == "angle relative drop");
            CHECK(std::string(MeasureLabelFor(adrop, Measure::RelativeDrop)) == "Relative drop" &&
                  std::string(MeasureLabelFor(still, Measure::Stillness)) == "Stillness");
            // A rotation's stillness / relative drop has no total: not offered; an odd record on
            // total reads total (as the engine does: it does not fit).
            CHECK(!RotationAxisOffered(Measure::Stillness, Axis::Total) &&
                  !RotationAxisOffered(Measure::RelativeDrop, Axis::Total));
            CHECK(RotationAxisOffered(Measure::Stillness, Axis::X) && RotationAxisOffered(Measure::RelativeDrop, Axis::Z));
            SignalSpec r = adrop;
            r.axis = Axis::Total;
            CHECK(RotationAxisOf(r) == Axis::Total);
            r.axis = Axis::Vertical;
            CHECK(RotationAxisOf(r) == Axis::Y);
        }
        CHECK(std::string(RotationReferenceLabel(Reference::Parent)) == "its parent" &&
              std::string(RotationReferenceLabel(Reference::Floor)) == "the world");

        Block b = DefaultBlock("Step", 0x1000000u | 0x5F9EDDu);
        CHECK(b.enabled && b.marker == "Step" && b.conditions.size() == 1 && b.landing == Landing::Crossing);
        CHECK(b.conditions[0].signal.bones == std::vector<int>{kDefaultConditionBone});
        CHECK(Near(b.conditions[0].threshold, 0.05) && b.conditions[0].auto_threshold);
        CHECK(PlacementOf(b) == Placement::Start);
        b.conditions.push_back(DefaultCondition());
        b.conditions.push_back(DefaultCondition());
        SetPlacement(b, Placement::Lowest, 2);
        CHECK(b.landing == Landing::PeakOf && !b.peak_max && b.peak_condition == 2);
        CHECK(PlacementOf(b) == Placement::Lowest);
        SetPlacement(b, Placement::Highest, 9);  // out of range: the first signal
        CHECK(b.peak_max && b.peak_condition == 0 && PlacementOf(b) == Placement::Highest);
        SetPlacement(b, Placement::Highest, 2);
        RemoveCondition(b, 0);  // the peak's signal moves down one place
        CHECK(b.conditions.size() == 2 && b.peak_condition == 1);
        RemoveCondition(b, 1);  // the peak's own signal goes: the first one
        CHECK(b.conditions.size() == 1 && b.peak_condition == 0);
        RemoveCondition(b, 5);  // out of range: nothing
        CHECK(b.conditions.size() == 1);
        SetPlacement(b, Placement::Start, 0);
        CHECK(b.landing == Landing::Crossing && PlacementOf(b) == Placement::Start);
    }

    // ---- Binding to tracks, Analyse's values back into the record ---------------------------
    {
        // Bound blocks hold skeleton bone indices: 40, 7, 40 (a reference), 12 (strength).
        Block b = DefaultBlock("Step", 0);
        b.conditions[0].signal.bones = {40, 7};
        Condition c2 = DefaultCondition();
        c2.signal.bones = {40};
        c2.signal.reference = Reference::Bones;
        c2.signal.ref_bones = {7};
        b.conditions.push_back(c2);
        b.strength_signal.bones = {12};
        Condition c3 = DefaultCondition();
        c3.signal.bones = {5};
        c3.signal.ref_bones = {99};  // reference = floor: its ref_bones are not read
        b.conditions.push_back(c3);
        std::vector<Block> bound = {b};
        std::vector<int> bones;
        RemapToTracks(bound, &bones);
        CHECK((bones == std::vector<int>{40, 7, 5, 12}));
        CHECK((bound[0].conditions[0].signal.bones == std::vector<int>{0, 1}));
        CHECK((bound[0].conditions[1].signal.bones == std::vector<int>{0}));
        CHECK((bound[0].conditions[1].signal.ref_bones == std::vector<int>{1}));
        CHECK((bound[0].conditions[2].signal.bones == std::vector<int>{2}));
        CHECK((bound[0].conditions[2].signal.ref_bones == std::vector<int>{99}));
        CHECK((bound[0].strength_signal.bones == std::vector<int>{3}));

        // Analyse's proposal goes back into the record; a Fixed condition keeps its own.
        std::vector<Block> record = {b};
        record[0].conditions[1].auto_threshold = false;
        record[0].conditions[1].threshold = 0.3;
        record[0].conditions[1].margin = 0.01;
        std::vector<Block> analysed = bound;
        for (Condition& c : analysed[0].conditions) {
            c.threshold = 0.123;
            c.margin = 0.045;
            c.signal.floor_y = 0.02;
            c.signal.bone_floors = {0.01, 0.03};
        }
        analysed[0].strength_signal.floor_y = 0.07;
        CopyAnalysedValues(analysed, record);
        CHECK(Near(record[0].conditions[0].threshold, 0.123) && Near(record[0].conditions[0].margin, 0.045));
        CHECK(Near(record[0].conditions[0].signal.floor_y, 0.02));
        CHECK((record[0].conditions[0].signal.bone_floors == std::vector<double>{0.01, 0.03}));
        CHECK((record[0].conditions[0].signal.bones == std::vector<int>{40, 7}));  // references stay
        CHECK(Near(record[0].conditions[1].threshold, 0.3) && Near(record[0].conditions[1].margin, 0.01));
        CHECK(Near(record[0].conditions[1].signal.floor_y, 0.0));
        CHECK(Near(record[0].strength_signal.floor_y, 0.07));
        CHECK((record[0].strength_signal.bones == std::vector<int>{12}));
    }

    // ---- Clip time on the project timeline -----------------------------------------------
    {
        // Item at 10 s, 4 s long, clip 3 s, no loop, start offset 0.5, rate 1: clip [0.5, 3)
        // shows over [10, 12.5).
        ItemClipMap m;
        m.item_pos = 10.0;
        m.item_len = 4.0;
        m.start_offs = 0.5;
        m.clip_len = 3.0;
        std::vector<ClipPass> p = ClipPasses(m);
        CHECK(p.size() == 1);
        if (p.size() == 1) CHECK(Near(p[0].p0, 10.0) && Near(p[0].p1, 12.5) && Near(p[0].clip0, 0.5));
        std::vector<double> t = ClipToProjectTimes(m, 1.5);
        CHECK(t.size() == 1 && Near(t[0], 11.0));
        CHECK(ClipToProjectTimes(m, 0.2).empty());  // trimmed away by the start offset
        double c = -1.0;
        CHECK(ProjectToClipTime(m, 11.0, &c) && Near(c, 1.5));
        CHECK(!ProjectToClipTime(m, 13.0, &c));  // past the clip's end (no loop)
        CHECK(!ProjectToClipTime(m, 9.0, &c));

        // Rate 2: the clip runs twice as fast.
        m.rate = 2.0;
        t = ClipToProjectTimes(m, 1.5);
        CHECK(t.size() == 1 && Near(t[0], 10.5));
        CHECK(ProjectToClipTime(m, 10.5, &c) && Near(c, 1.5));

        // Looped: every pass inside the item. Offset 0.5, rate 1, item 7 s, clip 3 s:
        // source [0.5, 7.5) = passes [10, 12.5) clip 0.5.., [12.5, 15.5) clip 0.., [15.5, 17) clip 0..
        m.rate = 1.0;
        m.loop = true;
        m.item_len = 7.0;
        p = ClipPasses(m);
        CHECK(p.size() == 3);
        if (p.size() == 3) {
            CHECK(Near(p[0].p0, 10.0) && Near(p[0].p1, 12.5) && Near(p[0].clip0, 0.5));
            CHECK(Near(p[1].p0, 12.5) && Near(p[1].p1, 15.5) && Near(p[1].clip0, 0.0));
            CHECK(Near(p[2].p0, 15.5) && Near(p[2].p1, 17.0) && Near(p[2].clip0, 0.0));
        }
        t = ClipToProjectTimes(m, 1.0);
        CHECK(t.size() == 3);
        if (t.size() == 3) CHECK(Near(t[0], 10.5) && Near(t[1], 13.5) && Near(t[2], 16.5));
        t = ClipToProjectTimes(m, 2.0);  // the last pass ends at clip 1.5
        CHECK(t.size() == 2);
        CHECK(ProjectToClipTime(m, 14.0, &c) && Near(c, 1.5));

        // Nothing to show.
        ItemClipMap z;
        CHECK(ClipPasses(z).empty());
        z.item_len = 1.0;
        z.clip_len = 1.0;
        z.start_offs = 5.0;  // past the clip, no loop
        CHECK(ClipPasses(z).empty());
    }

    // ---- Sample positions in a pass, consistent with ProjectToClipTime ----------------------
    {
        const double hz = 240.0;
        ItemClipMap m;
        m.item_pos = 10.0;
        m.item_len = 7.0;
        m.start_offs = 0.5;
        m.clip_len = 3.0;
        m.loop = true;
        for (double rate : {1.0, 2.0}) {
            m.rate = rate;
            const std::vector<ClipPass> passes = ClipPasses(m);
            CHECK(passes.size() >= 2);
            for (const ClipPass& p : passes)
                for (double f : {0.0, 0.3, 0.77}) {
                    const double t = p.p0 + (p.p1 - p.p0) * f;
                    double c = -1.0;
                    CHECK(ProjectToClipTime(m, t, &c));
                    CHECK(Near(SamplePosInPass(p, t, rate, hz), c * hz, 1e-6));
                }
        }
        // Rate 1, a later loop pass: project 13.5 is clip 1.0 of the second pass.
        m.rate = 1.0;
        const std::vector<ClipPass> p1 = ClipPasses(m);
        CHECK(Near(SamplePosInPass(p1[1], 13.5, 1.0, hz), 240.0, 1e-6));
        // Rate 2, first pass: project 10.5 is clip 0.5 + 0.5 * 2 = 1.5.
        m.rate = 2.0;
        CHECK(Near(SamplePosInPass(ClipPasses(m)[0], 10.5, 2.0, hz), 360.0, 1e-6));
        // Bad rate reads as 1.
        CHECK(Near(SamplePosInPass(p1[0], 10.5, 0.0, hz), 240.0, 1e-6));

        const std::vector<double> s = {0.0, 10.0, 20.0};
        CHECK(Near(InterpSample(s, 0.5), 5.0) && Near(InterpSample(s, 1.25), 12.5));
        CHECK(Near(InterpSample(s, -3.0), 0.0) && Near(InterpSample(s, 9.0), 20.0));
        CHECK(Near(InterpSample({}, 1.0), 0.0));
    }

    if (g_fails) {
        std::printf("tagging_signal_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("tagging_signal_test: all passed\n");
    return 0;
}
