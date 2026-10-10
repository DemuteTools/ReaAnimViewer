// SPDX-License-Identifier: MIT
//
// See auto_detect.h.

#include "auto_detect.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "bone_roles.h"
#include "event_list.h"   // KeepBlockEvents
#include "rule_record.h"  // BlocksEqual

namespace rav {
namespace {

constexpr AutoKind kKinds[kAutoKindCount] = {AutoKind::Step,  AutoKind::Heel,  AutoKind::Toe,     AutoKind::Lift,
                                             AutoKind::Slide, AutoKind::Pivot, AutoKind::Grab,    AutoKind::Release,
                                             AutoKind::HandPivot, AutoKind::HandPivotAny};
constexpr char     kSides[2] = {'L', 'R'};
// The analyses a session keeps for one set of role tracks (a few sensitivities tried in a row).
constexpr size_t kMaxCachedAnalyses = 8;

int SideIndex(char side)
{
    return side == 'R' ? 1 : 0;
}

// The kinds a ticked type makes, in block order.
std::vector<AutoKind> KindsOf(AutoType t, const AutoSettings& s)
{
    switch (t) {
    case AutoType::Step:
        return s.separate_steps ? std::vector<AutoKind>{AutoKind::Heel, AutoKind::Toe} : std::vector<AutoKind>{AutoKind::Step};
    case AutoType::LiftOff: return {AutoKind::Lift};
    case AutoType::Slide: return {AutoKind::Slide};
    case AutoType::Pivot: return {AutoKind::Pivot};
    case AutoType::Grab: return {AutoKind::Grab};
    case AutoType::Release: return {AutoKind::Release};
    case AutoType::HandPivot: return {s.hand_pivot_any ? AutoKind::HandPivotAny : AutoKind::HandPivot};
    }
    return {};
}

bool HasRole(const std::vector<int>& role_to_bone, Role r)
{
    const size_t i = static_cast<size_t>(r);
    return i < role_to_bone.size() && role_to_bone[i] >= 0;
}

void PushOnce(std::vector<std::string>& out, const std::string& s)
{
    if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
}

}  // namespace

const char* AutoTypeLabel(AutoType t)
{
    switch (t) {
    case AutoType::Step: return "Step";
    case AutoType::LiftOff: return "Lift-off";
    case AutoType::Slide: return "Slide scuff";
    case AutoType::Pivot: return "Pivot scuff";
    case AutoType::Grab: return "Grab";
    case AutoType::Release: return "Release";
    case AutoType::HandPivot: return "Pivot scuff";
    }
    return "?";
}

AutoCategory AutoCategoryOf(AutoType t)
{
    switch (t) {
    case AutoType::Step:
    case AutoType::LiftOff:
    case AutoType::Slide:
    case AutoType::Pivot: return AutoCategory::Feet;
    case AutoType::Grab:
    case AutoType::Release:
    case AutoType::HandPivot: return AutoCategory::Hands;
    }
    return AutoCategory::Feet;
}

const char* AutoCategoryLabel(AutoCategory c)
{
    switch (c) {
    case AutoCategory::Feet: return "Feet";
    case AutoCategory::Hands: return "Hands";
    }
    return "?";
}

const char* AutoKindWord(AutoKind k)
{
    switch (k) {
    case AutoKind::Step: return "step";
    case AutoKind::Heel: return "heel";
    case AutoKind::Toe: return "toe";
    case AutoKind::Lift: return "lift";
    case AutoKind::Slide: return "slide";
    case AutoKind::Pivot: return "pivot";
    case AutoKind::Grab: return "grab";
    case AutoKind::Release: return "release";
    case AutoKind::HandPivot: return "hand_pivot";
    case AutoKind::HandPivotAny: return "hand_pivot_any";
    }
    return "?";
}

bool AutoKindFromWord(const std::string& w, AutoKind* out)
{
    for (AutoKind k : kKinds)
        if (w == AutoKindWord(k)) {
            if (out) *out = k;
            return true;
        }
    return false;
}

AutoType AutoTypeOfKind(AutoKind k)
{
    switch (k) {
    case AutoKind::Step:
    case AutoKind::Heel:
    case AutoKind::Toe: return AutoType::Step;
    case AutoKind::Lift: return AutoType::LiftOff;
    case AutoKind::Slide: return AutoType::Slide;
    case AutoKind::Pivot: return AutoType::Pivot;
    case AutoKind::Grab: return AutoType::Grab;
    case AutoKind::Release: return AutoType::Release;
    case AutoKind::HandPivot:
    case AutoKind::HandPivotAny: return AutoType::HandPivot;
    }
    return AutoType::Step;
}

std::string AutoMarkerName(AutoKind k, char side)
{
    const char* word = "?";
    switch (k) {
    case AutoKind::Step: word = "FS"; break;
    case AutoKind::Heel: word = "Heel"; break;
    case AutoKind::Toe: word = "Toe"; break;
    case AutoKind::Lift: word = "Lift"; break;
    case AutoKind::Slide: word = "Slide"; break;
    case AutoKind::Pivot: word = "Pivot"; break;
    case AutoKind::Grab: word = "Grab"; break;
    case AutoKind::Release: word = "Release"; break;
    case AutoKind::HandPivot:
    case AutoKind::HandPivotAny: word = "Hand Pivot"; break;
    }
    return std::string(word) + ' ' + (side == 'R' ? 'R' : 'L');
}

uint32_t AutoDefaultColor(AutoKind k, char side)
{
    // Left in a cool hue, right in a warm one (the Footsteps presets' blue and orange for steps).
    static const uint32_t kRgb[kAutoKindCount][2] = {
        {0x5F9EDD, 0xDD9E5F},  // FS
        {0x5F9EDD, 0xDD9E5F},  // Heel
        {0x5FDDDD, 0xDDDD5F},  // Toe
        {0x5F5FDD, 0xDD5F5F},  // Lift
        {0x5FDD5F, 0xDD5FDD},  // Slide
        {0x9E5FDD, 0xDD5F9E},  // Pivot
        {0x5FDD9E, 0xDDB45F},  // Grab
        {0x9EB4DD, 0xDD9E9E},  // Release
        {0xA8DD3C, 0xB4643C},  // Hand Pivot
        {0xA8DD3C, 0xB4643C},  // Hand Pivot (any resting hand)
    };
    const int ki = std::clamp(static_cast<int>(k), 0, kAutoKindCount - 1);
    return 0x1000000u | kRgb[ki][SideIndex(side)];
}

bool AutoBlockKind(const Block& b, AutoKind* kind, char* side)
{
    if (!IsAutoBlock(b) || (b.auto_side != 'L' && b.auto_side != 'R')) return false;
    AutoKind k = AutoKind::Step;
    if (!AutoKindFromWord(b.auto_type, &k)) return false;
    if (kind) *kind = k;
    if (side) *side = b.auto_side;
    return true;
}

Block MakeAutoBlock(AutoKind k, char side, double sens, double offset_ms)
{
    Block b;
    b.auto_type = AutoKindWord(k);
    b.auto_side = side == 'R' ? 'R' : 'L';
    b.sens = sens;
    b.offset_ms = offset_ms;
    b.marker = AutoMarkerName(k, b.auto_side);
    b.color = AutoDefaultColor(k, b.auto_side);
    return b;
}

// ---- Settings ----------------------------------------------------------------------------------

const AutoRowValue* FindAutoRow(const AutoTypeSettings& t, const std::string& kind, char side)
{
    for (const AutoRowValue& r : t.rows)
        if (r.kind == kind && r.side == side) return &r;
    return nullptr;
}

void SetAutoRowValue(AutoTypeSettings& t, const std::string& kind, char side, bool sens, double v)
{
    AutoRowValue* row = nullptr;
    for (AutoRowValue& r : t.rows)
        if (r.kind == kind && r.side == side) row = &r;
    if (!row) {
        t.rows.push_back(AutoRowValue{kind, side, t.sens, t.offset_ms});
        row = &t.rows.back();
    }
    (sens ? row->sens : row->offset_ms) = v;
}

bool operator==(const AutoTypeSettings& a, const AutoTypeSettings& b)
{
    return a.on == b.on && a.sens == b.sens && a.offset_ms == b.offset_ms;
}

bool operator==(const AutoSettings& a, const AutoSettings& b)
{
    for (int t = 0; t < kAutoTypeCount; ++t)
        if (!(a.type[t] == b.type[t])) return false;
    return a.separate_steps == b.separate_steps && a.hand_pivot_any == b.hand_pivot_any;
}

bool AutoCategoryInBlocks(const std::vector<Block>& blocks, AutoCategory c)
{
    for (const Block& b : blocks) {
        AutoKind k;
        if (AutoBlockKind(b, &k, nullptr) && AutoCategoryOf(AutoTypeOfKind(k)) == c) return true;
    }
    return false;
}

int AutoCategoryOnCount(const AutoSettings& s, AutoCategory c)
{
    int n = 0;
    for (int t = 0; t < kAutoTypeCount; ++t)
        if (s.type[t].on && AutoCategoryOf(static_cast<AutoType>(t)) == c) ++n;
    return n;
}

void UntickAutoCategory(AutoSettings& s, AutoCategory c)
{
    for (int t = 0; t < kAutoTypeCount; ++t)
        if (AutoCategoryOf(static_cast<AutoType>(t)) == c) s.type[t].on = false;
}

bool AutoPending(const AutoSettings& ui, const AutoSettings& item)
{
    for (int t = 0; t < kAutoTypeCount; ++t)
        if (ui.type[t].on != item.type[t].on) return true;
    const int step = static_cast<int>(AutoType::Step), hp = static_cast<int>(AutoType::HandPivot);
    return (ui.type[step].on && item.type[step].on && ui.separate_steps != item.separate_steps) ||
           (ui.type[hp].on && item.type[hp].on && ui.hand_pivot_any != item.hand_pivot_any);
}

std::vector<Block> AutoBlocks(const AutoSettings& s)
{
    std::vector<Block> out;
    for (int t = 0; t < kAutoTypeCount; ++t) {
        const AutoTypeSettings& ts = s.type[t];
        if (!ts.on) continue;
        for (AutoKind k : KindsOf(static_cast<AutoType>(t), s))
            for (char side : kSides) {
                const AutoRowValue* row = FindAutoRow(ts, AutoKindWord(k), side);
                out.push_back(MakeAutoBlock(k, side, row ? row->sens : ts.sens, row ? row->offset_ms : ts.offset_ms));
            }
    }
    return out;
}

AutoSettings ReadAutoSettings(const std::vector<Block>& blocks)
{
    AutoSettings s;
    bool combined = false, separate = false, strict = false, any = false;
    for (const Block& b : blocks) {
        AutoKind k;
        if (!AutoBlockKind(b, &k, nullptr)) continue;
        const int t = static_cast<int>(AutoTypeOfKind(k));
        if (!s.type[t].on) {
            s.type[t].on = true;
            s.type[t].sens = b.sens;
            s.type[t].offset_ms = b.offset_ms;
        }
        if (k == AutoKind::Step) combined = true;
        if (k == AutoKind::Heel || k == AutoKind::Toe) separate = true;
        if (k == AutoKind::HandPivot) strict = true;
        if (k == AutoKind::HandPivotAny) any = true;
    }
    s.separate_steps = separate && !combined;
    s.hand_pivot_any = any && !strict;
    return s;
}

std::vector<int> ApplyAutoSettings(std::vector<Block>& blocks, const AutoSettings& s, bool* changed)
{
    const std::vector<Block> old = blocks;
    const std::vector<Block> want = AutoBlocks(s);
    std::vector<int>         match_of_want(want.size(), -1);  // the old block each wanted one keeps
    std::vector<int>         want_of_old(old.size(), -1);
    std::vector<char>        keep(old.size(), 0);
    for (size_t i = 0; i < old.size(); ++i) {
        AutoKind k;
        char     side;
        if (!AutoBlockKind(old[i], &k, &side)) {
            keep[i] = 1;  // a rule, or an auto type this version does not know
            continue;
        }
        for (size_t j = 0; j < want.size(); ++j) {
            AutoKind wk;
            char     ws;
            AutoBlockKind(want[j], &wk, &ws);
            if (match_of_want[j] < 0 && wk == k && ws == side) {
                match_of_want[j] = static_cast<int>(i);
                want_of_old[i] = static_cast<int>(j);
                keep[i] = 1;
                break;
            }
        }
    }
    // Story 10-8e: a hand pivot block switched between after a grab and any keeps its place,
    // marker, colour and on/off; only its kind changes.
    auto hand_pivot = [](AutoKind k) { return k == AutoKind::HandPivot || k == AutoKind::HandPivotAny; };
    for (size_t i = 0; i < old.size(); ++i) {
        AutoKind k;
        char     side;
        if (keep[i] || !AutoBlockKind(old[i], &k, &side) || !hand_pivot(k)) continue;
        for (size_t j = 0; j < want.size(); ++j) {
            AutoKind wk;
            char     ws;
            AutoBlockKind(want[j], &wk, &ws);
            if (match_of_want[j] < 0 && hand_pivot(wk) && ws == side) {
                match_of_want[j] = static_cast<int>(i);
                want_of_old[i] = static_cast<int>(j);
                keep[i] = 1;
                break;
            }
        }
    }
    std::vector<Block> out;
    std::vector<int>   map(old.size(), -1);
    for (size_t i = 0; i < old.size(); ++i) {
        if (!keep[i]) continue;
        Block b = old[i];
        if (want_of_old[i] >= 0) {
            const Block& w = want[static_cast<size_t>(want_of_old[i])];
            // Story 10-8h: a kept block keeps its own sensitivity and offset (per side or part,
            // perhaps raised by Detect). Its kind switched (hand pivot): a raise found for the
            // other kind goes; it starts again from the value it was raised from.
            if (b.auto_type != w.auto_type && b.sens_from >= 0.0) {
                b.sens = b.sens_from;
                b.sens_from = -1.0;
            }
            b.auto_type = w.auto_type;
        }
        map[i] = static_cast<int>(out.size());
        out.push_back(std::move(b));
    }
    for (size_t j = 0; j < want.size(); ++j) {
        if (match_of_want[j] >= 0) continue;
        Block nb = want[j];
        // Story 10-8h: a new block replacing a dropped block of the same type and side (steps
        // switched between combined and separate) takes that block's start value (the value it
        // was raised from, else its own) and offset, not raised; Detect's search raises it again
        // if needed. Else the type's value.
        AutoKind wk;
        char     ws;
        AutoBlockKind(nb, &wk, &ws);
        for (size_t i = 0; i < old.size(); ++i) {
            AutoKind k;
            char     side;
            if (keep[i] || !AutoBlockKind(old[i], &k, &side) || side != ws || AutoTypeOfKind(k) != AutoTypeOfKind(wk))
                continue;
            nb.sens = old[i].sens_from >= 0.0 ? old[i].sens_from : old[i].sens;
            nb.offset_ms = old[i].offset_ms;
            nb.sens_from = -1.0;
            break;
        }
        out.push_back(std::move(nb));
    }
    // Steps switched between separate and combined: the dropped step blocks' edits go to the new
    // step block of the same foot (combined -> heel, heel and toe -> combined). (A switched hand
    // pivot keeps its block, above; these two lines cover a hand left with both kinds.)
    auto find_new = [&](AutoKind k, char side) {
        for (size_t n = 0; n < out.size(); ++n) {
            AutoKind nk;
            char     ns;
            if (AutoBlockKind(out[n], &nk, &ns) && nk == k && ns == side) return static_cast<int>(n);
        }
        return -1;
    };
    for (size_t i = 0; i < old.size(); ++i) {
        AutoKind k;
        char     side;
        if (keep[i] || !AutoBlockKind(old[i], &k, &side)) continue;
        if (k == AutoKind::Step) map[i] = find_new(AutoKind::Heel, side);
        else if (k == AutoKind::Heel || k == AutoKind::Toe) map[i] = find_new(AutoKind::Step, side);
        else if (k == AutoKind::HandPivot) map[i] = find_new(AutoKind::HandPivotAny, side);
        else if (k == AutoKind::HandPivotAny) map[i] = find_new(AutoKind::HandPivot, side);
    }
    if (changed) *changed = !BlocksEqual(old, out);
    blocks = std::move(out);
    return map;
}

// ---- Physics parameters ------------------------------------------------------------------------

double SensitivityFactor(double sens)
{
    if (!std::isfinite(sens)) sens = kAutoDefaultSens;
    sens = std::clamp(sens, 0.0, 100.0);
    return std::pow(2.0, (sens - 50.0) / 50.0);
}

PhysicsParams AutoPhysicsParams(AutoType t, double sens)
{
    PhysicsParams p;
    const double  f = SensitivityFactor(sens);
    switch (t) {
    case AutoType::Step:
    case AutoType::LiftOff: p.contact_speed *= f; break;
    case AutoType::Slide: p.slide_speed /= f; break;
    case AutoType::Pivot:
        p.pivot_min_deg /= f;
        p.pivot_rate_dps /= f;
        break;
    case AutoType::Grab:
    case AutoType::Release: p.hand_contact_speed *= f; break;
    case AutoType::HandPivot:
        p.hand_pivot_min_deg /= f;
        p.hand_pivot_rate_dps /= f;
        break;
    }
    return p;
}

bool SameAutoAnalysis(const PhysicsParams& a, const PhysicsParams& b)
{
    return a.contact_speed == b.contact_speed && a.slide_speed == b.slide_speed && a.pivot_min_deg == b.pivot_min_deg &&
           a.pivot_rate_dps == b.pivot_rate_dps && a.hand_contact_speed == b.hand_contact_speed &&
           a.hand_switch_cost == b.hand_switch_cost && a.hand_pivot_min_deg == b.hand_pivot_min_deg &&
           a.hand_pivot_rate_dps == b.hand_pivot_rate_dps;
}

// ---- Detection ---------------------------------------------------------------------------------

bool IsActiveAutoBlock(const Block& b)
{
    return b.enabled && AutoBlockKind(b, nullptr, nullptr);
}

bool HasActiveAutoBlocks(const std::vector<Block>& blocks)
{
    for (const Block& b : blocks)
        if (IsActiveAutoBlock(b)) return true;
    return false;
}

std::vector<PhysicsParams> AutoParamSets(const std::vector<Block>& blocks)
{
    std::vector<PhysicsParams> out;
    for (const Block& b : blocks) {
        AutoKind k;
        if (!b.enabled || !AutoBlockKind(b, &k, nullptr)) continue;
        const PhysicsParams p = AutoPhysicsParams(AutoTypeOfKind(k), b.sens);
        bool                seen = false;
        for (const PhysicsParams& q : out) seen = seen || SameAutoAnalysis(p, q);
        if (!seen) out.push_back(p);
    }
    return out;
}

std::vector<Event> AutoEvents(const std::vector<AutoAnalysis>& analyses, const std::vector<Block>& blocks)
{
    std::vector<Event> out;
    // Each analysis' foot and hand events, computed once (the hand's twice: pivots after a grab
    // only, and of any resting hand).
    std::vector<std::vector<PhysicsEvent>> foot(analyses.size()), hand(analyses.size()), hand_any(analyses.size());
    std::vector<char>                      done(analyses.size(), 0);
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        const Block& b = blocks[bi];
        AutoKind     k;
        char         side;
        if (!b.enabled || !AutoBlockKind(b, &k, &side)) continue;
        const PhysicsParams p = AutoPhysicsParams(AutoTypeOfKind(k), b.sens);
        size_t              ai = analyses.size();
        for (size_t j = 0; j < analyses.size() && ai == analyses.size(); ++j)
            if (SameAutoAnalysis(analyses[j].params, p)) ai = j;
        if (ai == analyses.size()) continue;
        const PhysicsAnalysis& a = analyses[ai].analysis;
        if (!a.ok) continue;
        if (!done[ai]) {
            foot[ai] = FootEvents(a, analyses[ai].params);
            hand[ai] = HandEvents(a, analyses[ai].params);
            PhysicsParams any = analyses[ai].params;
            any.hand_pivot_after_grab = false;
            hand_any[ai] = HandEvents(a, any);
            done[ai] = 1;
        }
        const bool                       is_hand = AutoCategoryOf(AutoTypeOfKind(k)) == AutoCategory::Hands;
        // Separate toe steps: the ball, or the tip on a rig without a ball bone.
        const FootTrack& ft = a.foot[SideIndex(side)];
        const int        toe = ft.part[static_cast<int>(FootPart::Ball)].present ? static_cast<int>(FootPart::Ball)
                                                                                  : static_cast<int>(FootPart::Tip);
        const bool pivot = k == AutoKind::Pivot || k == AutoKind::HandPivot || k == AutoKind::HandPivotAny;
        for (const PhysicsEvent& e : !is_hand ? foot[ai] : k == AutoKind::HandPivotAny ? hand_any[ai] : hand[ai]) {
            if (e.side != side) continue;
            bool want = false;
            switch (k) {
            case AutoKind::Step: want = e.kind == PhysicsKind::FootStep; break;
            case AutoKind::Heel: want = e.kind == PhysicsKind::Step && e.part == static_cast<int>(FootPart::Heel); break;
            case AutoKind::Toe: want = e.kind == PhysicsKind::Step && e.part == toe; break;
            case AutoKind::Lift: want = e.kind == PhysicsKind::LiftOff; break;
            case AutoKind::Slide: want = e.kind == PhysicsKind::Slide; break;
            case AutoKind::Pivot: want = e.kind == PhysicsKind::Pivot; break;
            case AutoKind::Grab: want = e.kind == PhysicsKind::Grab; break;
            case AutoKind::Release: want = e.kind == PhysicsKind::Release; break;
            case AutoKind::HandPivot:
            case AutoKind::HandPivotAny: want = e.kind == PhysicsKind::HandPivot; break;
            }
            if (!want) continue;
            Event ev;
            ev.time_s = e.time_s + b.offset_ms / 1000.0;
            ev.block = static_cast<int>(bi);
            ev.marker = b.marker;
            ev.strength = e.strength;
            ev.speed = pivot ? 0.0 : e.strength * a.leg_length;
            out.push_back(std::move(ev));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Event& x, const Event& y) {
        return std::make_tuple(x.time_s, x.block) < std::make_tuple(y.time_s, y.block);
    });
    return out;
}

std::vector<Event> DetectAutoEvents(const std::vector<Block>& blocks, const std::vector<BoneTrack>& role_tracks,
                                    std::vector<AutoAnalysis>* cache)
{
    if (!HasActiveAutoBlocks(blocks)) return {};
    std::vector<AutoAnalysis>  local;
    std::vector<AutoAnalysis>& runs = cache ? *cache : local;
    const std::vector<PhysicsParams> sets = AutoParamSets(blocks);
    for (const PhysicsParams& p : sets) {
        bool have = false;
        for (const AutoAnalysis& r : runs) have = have || SameAutoAnalysis(r.params, p);
        if (have) continue;
        AutoAnalysis r;
        r.params = p;
        r.analysis = AnalyseMotion(role_tracks, p);
        runs.push_back(std::move(r));
    }
    // A cache keeps the analyses in use and the latest others, a few at most.
    if (cache) {
        for (size_t i = 0; i < runs.size() && runs.size() > std::max(kMaxCachedAnalyses, sets.size());) {
            bool used = false;
            for (const PhysicsParams& p : sets) used = used || SameAutoAnalysis(runs[i].params, p);
            if (used) ++i;
            else runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    return AutoEvents(runs, blocks);
}

// ---- Sensitivity search (story 10-8h) ---------------------------------------------------------

namespace {

// Block `b`'s detected events at sensitivity `sens` (-1 = its analysis failed), on `runs`.
int CountAt(const Block& b, double sens, const std::vector<BoneTrack>& role_tracks, std::vector<AutoAnalysis>& runs)
{
    Block one = b;
    one.sens = sens;
    const std::vector<Block> blocks = {one};
    const std::vector<Event> ev = DetectAutoEvents(blocks, role_tracks, &runs);
    AutoKind k;
    AutoBlockKind(one, &k, nullptr);
    const PhysicsParams p = AutoPhysicsParams(AutoTypeOfKind(k), sens);
    for (const AutoAnalysis& r : runs)
        if (SameAutoAnalysis(r.params, p) && !r.analysis.ok) return -1;
    return static_cast<int>(ev.size());
}

}  // namespace

std::vector<AutoSensResult> SearchAutoSensitivity(const std::vector<Block>& blocks, const std::vector<BoneTrack>& role_tracks,
                                                  const std::vector<int>& event_counts, const std::vector<int>& role_to_bone)
{
    std::vector<AutoSensResult> out(blocks.size());
    // A local cache (every analysis of the search; the session's 8-entry one is left alone). The
    // two sides of one type share theirs.
    std::vector<AutoAnalysis> runs;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        AutoSensResult& r = out[i];
        r.sens = b.sens;
        if (!IsActiveAutoBlock(b) || AutoBlockPartMissing(b, role_to_bone)) continue;
        if (i >= event_counts.size() || event_counts[i] != 0) continue;  // has events (or unknown): untouched
        const double start = std::isfinite(b.sens) ? std::clamp(b.sens, 0.0, 100.0) : kAutoDefaultSens;
        if (CountAt(b, start, role_tracks, runs) != 0) continue;  // a failed analysis, or events after all
        const int at100 = CountAt(b, 100.0, role_tracks, runs);
        if (at100 < 0) continue;
        r.searched = true;
        if (at100 == 0) {
            r.none = true;
            continue;
        }
        // Bisection: lo has no event, hi has some; down to the lowest whole % (about 7 analyses).
        double lo = start, hi = 100.0;
        while (hi - lo > 1.0) {
            const double mid = std::floor((lo + hi) * 0.5 + 0.5);
            if (mid <= lo || mid >= hi) break;
            const int n = CountAt(b, mid, role_tracks, runs);
            if (n > 0) hi = mid;
            else lo = mid;  // none, or a failed analysis there: search higher
        }
        r.sens = hi;
        r.raised = true;
        r.sens_from = b.sens_from >= 0.0 ? b.sens_from : start;
    }
    return out;
}

bool ApplyAutoSensSearch(std::vector<Block>& blocks, const std::vector<BoneTrack>& role_tracks,
                         const std::vector<int>& role_to_bone, std::vector<AutoSensResult>* results,
                         std::vector<AutoAnalysis>* cache)
{
    std::vector<AutoSensResult> res;
    bool                        changed = false;
    if (HasActiveAutoBlocks(blocks)) {
        std::vector<int> counts(blocks.size(), 0);
        for (const Event& e : DetectAutoEvents(blocks, role_tracks, cache))
            if (e.block >= 0 && e.block < static_cast<int>(counts.size())) ++counts[static_cast<size_t>(e.block)];
        res = SearchAutoSensitivity(blocks, role_tracks, counts, role_to_bone);
        for (size_t i = 0; i < res.size() && i < blocks.size(); ++i) {
            Block& b = blocks[i];
            if (res[i].raised && (res[i].sens != b.sens || res[i].sens_from != b.sens_from)) {
                b.sens = res[i].sens;
                b.sens_from = res[i].sens_from;
                changed = true;
            }
        }
    }
    if (results) *results = std::move(res);
    return changed;
}

// ---- Missing parts -----------------------------------------------------------------------------

std::vector<std::string> AutoMissingParts(AutoType t, bool separate, const std::vector<int>& role_to_bone)
{
    std::vector<std::string> out;
    auto has = [&](Role r) { return HasRole(role_to_bone, r); };
    const Role knee[2] = {Role::LeftKnee, Role::RightKnee};
    const Role up_leg[2] = {Role::LeftUpLeg, Role::RightUpLeg};
    // Body scale: heel, knee and up leg (or the hips) of one side; else knee and up leg of one
    // side (twice the thigh).
    bool scale = false;
    for (int s = 0; s < 2; ++s)
        scale = scale || (has(FootPartRole(kSides[s], 0)) && has(knee[s]) && (has(up_leg[s]) || has(Role::Hips))) ||
                (has(knee[s]) && has(up_leg[s]));
    if (!scale)
        for (int s = 0; s < 2; ++s) {
            if (!has(FootPartRole(kSides[s], 0))) PushOnce(out, RoleName(FootPartRole(kSides[s], 0)));
            if (!has(knee[s])) PushOnce(out, RoleName(knee[s]));
            if (!has(up_leg[s]) && !has(Role::Hips)) PushOnce(out, RoleName(up_leg[s]));
        }
    if (AutoCategoryOf(t) == AutoCategory::Hands) {
        for (Role hand : {Role::LeftHand, Role::RightHand})
            if (!has(hand)) PushOnce(out, RoleName(hand));
        return out;
    }
    for (char side : kSides) {
        const Role heel = FootPartRole(side, 0), ball = FootPartRole(side, 1), tip = FootPartRole(side, 2);
        const bool any = has(heel) || has(ball) || has(tip);
        const bool toe = has(ball) || has(tip);
        if (t == AutoType::Pivot || (t == AutoType::Step && separate)) {
            if (!has(heel)) PushOnce(out, RoleName(heel));
            if (!toe) PushOnce(out, RoleName(ball));
        } else if (!any) {
            PushOnce(out, RoleName(heel));
            PushOnce(out, RoleName(ball));
            PushOnce(out, RoleName(tip));
        }
    }
    return out;
}

bool AutoBlockPartMissing(const Block& b, const std::vector<int>& role_to_bone)
{
    AutoKind kind;
    char     side = 'L';
    if (!b.enabled || !AutoBlockKind(b, &kind, &side)) return false;
    auto       has = [&](Role r) { return HasRole(role_to_bone, r); };
    const Role knee[2] = {Role::LeftKnee, Role::RightKnee};
    const Role up_leg[2] = {Role::LeftUpLeg, Role::RightUpLeg};
    bool       scale = false;  // as AutoMissingParts
    for (int s = 0; s < 2; ++s)
        scale = scale || (has(FootPartRole(kSides[s], 0)) && has(knee[s]) && (has(up_leg[s]) || has(Role::Hips))) ||
                (has(knee[s]) && has(up_leg[s]));
    if (!scale) return true;
    const Role heel = FootPartRole(side, 0), ball = FootPartRole(side, 1), tip = FootPartRole(side, 2);
    const bool toe = has(ball) || has(tip);
    switch (kind) {
    case AutoKind::Grab:
    case AutoKind::Release:
    case AutoKind::HandPivot:
    case AutoKind::HandPivotAny: return !has(side == 'R' ? Role::RightHand : Role::LeftHand);
    case AutoKind::Heel: return !has(heel);
    case AutoKind::Toe: return !toe;
    case AutoKind::Pivot: return !has(heel) || !toe;
    case AutoKind::Step:
    case AutoKind::Lift:
    case AutoKind::Slide: return !has(heel) && !toe;
    }
    return false;
}

std::vector<char> AutoBlocksPartMissing(const std::vector<Block>& blocks, const std::vector<int>& role_to_bone)
{
    std::vector<char> out(blocks.size(), 0);
    for (size_t i = 0; i < blocks.size(); ++i) out[i] = AutoBlockPartMissing(blocks[i], role_to_bone) ? 1 : 0;
    return out;
}

std::vector<char> KeepUnrunnableEvents(std::vector<Event>& detections, const std::vector<EventEntry>& entries,
                                       const std::vector<Block>& blocks, const std::vector<char>& skipped,
                                       const std::vector<int>& role_to_bone)
{
    std::vector<char> keep = AutoBlocksPartMissing(blocks, role_to_bone);
    for (size_t i = 0; i < keep.size() && i < skipped.size(); ++i) keep[i] = (keep[i] || skipped[i]) ? 1 : 0;
    KeepBlockEvents(detections, entries, blocks, keep);
    return keep;
}

}  // namespace rav
