// SPDX-License-Identifier: MIT
//
// See auto_detect.h.

#include "auto_detect.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "auto_types.h"  // story 10-9-0: the rows of the types and kinds
#include "bone_roles.h"
#include "event_list.h"   // KeepBlockEvents
#include "rule_record.h"  // BlocksEqual

namespace rav {
namespace {

constexpr char     kSides[2] = {'L', 'R'};
// The analyses a session keeps for one set of role tracks (a few sensitivities tried in a row).
constexpr size_t kMaxCachedAnalyses = 8;

int SideIndex(char side)
{
    return side == 'R' ? 1 : 0;
}

// The kinds a ticked type makes, in block order. The modes of steps, slides, pivots and hand
// pivots pick among their kinds; a type with no mode makes all its kinds (story 10-9-0).
std::vector<AutoKind> KindsOf(AutoType t, const AutoSettings& s)
{
    switch (t) {
    case AutoType::Step:
        return s.separate_steps ? std::vector<AutoKind>{AutoKind::Heel, AutoKind::Toe} : std::vector<AutoKind>{AutoKind::Step};
    case AutoType::Slide:
        switch (s.slide_parts) {
        case ScuffParts::EachPart: return {AutoKind::SlideHeel, AutoKind::SlideToe};
        case ScuffParts::Longest: return {AutoKind::SlideHeelLongest, AutoKind::SlideToeLongest};
        case ScuffParts::Combined: break;
        }
        return {AutoKind::Slide};
    case AutoType::Pivot:
        switch (s.pivot_parts) {
        case ScuffParts::EachPart: return {AutoKind::PivotHeel, AutoKind::PivotToe};
        case ScuffParts::Longest: return {AutoKind::PivotHeelLongest, AutoKind::PivotToeLongest};
        case ScuffParts::Combined: break;
        }
        return {AutoKind::Pivot};
    case AutoType::HandPivot: return {s.hand_pivot_any ? AutoKind::HandPivotAny : AutoKind::HandPivot};
    default: break;
    }
    return AutoKindsOf(t);
}

// Story 10-8m: a slide / pivot part kind's mode (Combined for any other kind), and whether it is
// the heel's (the kind's row).
ScuffParts ScuffMode(AutoKind k)
{
    return AutoKindRow(k).scuff;
}

bool ScuffHeel(AutoKind k)
{
    return AutoKindRow(k).scuff_heel;
}

// The kinds that are one block switched between modes (place, marker, colour, edits kept): the hand
// pivots, and each slide / pivot part across Each part / Longest part. 0 = none.
int SwapGroup(AutoKind k)
{
    if (k == AutoKind::HandPivot || k == AutoKind::HandPivotAny) return 1;
    if (ScuffMode(k) == ScuffParts::Combined) return 0;
    return 2 + (AutoTypeOfKind(k) == AutoType::Pivot ? 2 : 0) + (ScuffHeel(k) ? 0 : 1);
}

bool InRange(int i, int count)
{
    return i >= 0 && i < count;
}

}  // namespace

const char* AutoTypeLabel(AutoType t)
{
    return InRange(static_cast<int>(t), kAutoTypeCount) ? AutoTypeRow(t).label : "?";
}

AutoCategory AutoCategoryOf(AutoType t)
{
    return AutoTypeRow(t).category;
}

const char* AutoCategoryLabel(AutoCategory c)
{
    return InRange(static_cast<int>(c), kAutoCategoryCount) ? AutoCategoryRow(c).label : "?";
}

const char* AutoKindWord(AutoKind k)
{
    return InRange(static_cast<int>(k), kAutoKindCount) ? AutoKindRow(k).word : "?";
}

bool AutoKindFromWord(const std::string& w, AutoKind* out)
{
    for (int i = 0; i < kAutoKindCount; ++i) {
        const AutoKind k = static_cast<AutoKind>(i);
        if (w == AutoKindWord(k)) {
            if (out) *out = k;
            return true;
        }
    }
    return false;
}

AutoType AutoTypeOfKind(AutoKind k)
{
    return AutoKindRow(k).type;
}

std::string AutoMarkerName(AutoKind k, char side)
{
    const bool  known = InRange(static_cast<int>(k), kAutoKindCount);
    const char* word = known ? AutoKindRow(k).marker : "?";
    if (known && AutoKindSideless(k)) return word;  // story 10-9a: "Jump", "Land"
    return std::string(word) + ' ' + (side == 'R' ? 'R' : 'L');
}

uint32_t AutoDefaultColor(AutoKind k, char side)
{
    const int      ki = std::clamp(static_cast<int>(k), 0, kAutoKindCount - 1);
    const AutoKind kk = static_cast<AutoKind>(ki);
    return 0x1000000u | AutoKindRow(kk).color[AutoKindSideless(kk) ? 0 : SideIndex(side)];
}

bool AutoKindSideless(AutoKind k)
{
    return InRange(static_cast<int>(k), kAutoKindCount) && AutoTypeRow(AutoKindRow(k).type).sideless;
}

const char* AutoKindSides(AutoKind k)
{
    return AutoKindSideless(k) ? "B" : "LR";
}

bool AutoBlockKind(const Block& b, AutoKind* kind, char* side)
{
    if (!IsAutoBlock(b)) return false;
    AutoKind k = AutoKind::Step;
    if (!AutoKindFromWord(b.auto_type, &k)) return false;
    // Story 10-9a: a sideless kind's block is side B, the others L or R.
    if (AutoKindSideless(k) ? b.auto_side != 'B' : (b.auto_side != 'L' && b.auto_side != 'R')) return false;
    if (kind) *kind = k;
    if (side) *side = b.auto_side;
    return true;
}

Block MakeAutoBlock(AutoKind k, char side, double sens, double offset_ms)
{
    Block b;
    b.auto_type = AutoKindWord(k);
    b.auto_side = AutoKindSideless(k) ? 'B' : (side == 'R' ? 'R' : 'L');
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
    return a.separate_steps == b.separate_steps && a.hand_pivot_any == b.hand_pivot_any &&
           a.slide_parts == b.slide_parts && a.pivot_parts == b.pivot_parts;
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
    const int sl = static_cast<int>(AutoType::Slide), pv = static_cast<int>(AutoType::Pivot);
    return (ui.type[step].on && item.type[step].on && ui.separate_steps != item.separate_steps) ||
           (ui.type[hp].on && item.type[hp].on && ui.hand_pivot_any != item.hand_pivot_any) ||
           (ui.type[sl].on && item.type[sl].on && ui.slide_parts != item.slide_parts) ||
           (ui.type[pv].on && item.type[pv].on && ui.pivot_parts != item.pivot_parts);
}

bool AutoTypeSeparate(AutoType t, const AutoSettings& s)
{
    switch (t) {
    case AutoType::Step: return s.separate_steps;
    case AutoType::Slide: return s.slide_parts != ScuffParts::Combined;
    case AutoType::Pivot: return s.pivot_parts != ScuffParts::Combined;
    default: break;
    }
    return false;
}

std::vector<Block> AutoBlocks(const AutoSettings& s)
{
    std::vector<Block> out;
    for (int t = 0; t < kAutoTypeCount; ++t) {
        const AutoTypeSettings& ts = s.type[t];
        if (!ts.on) continue;
        for (AutoKind k : KindsOf(static_cast<AutoType>(t), s))
            for (const char* sp = AutoKindSides(k); *sp; ++sp) {
                const char          side = *sp;
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
    bool scuff_mode[2][3] = {};  // story 10-8m: per scuff type (slide, pivot), the modes its blocks say
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
        if (t == static_cast<int>(AutoType::Slide) || t == static_cast<int>(AutoType::Pivot))
            scuff_mode[t == static_cast<int>(AutoType::Pivot) ? 1 : 0][static_cast<int>(ScuffMode(k))] = true;
    }
    for (int i = 0; i < 2; ++i) {
        const bool*      m = scuff_mode[i];
        const ScuffParts mode = (m[0] || (m[1] && m[2])) ? ScuffParts::Combined
                                : m[1]                   ? ScuffParts::EachPart
                                : m[2]                   ? ScuffParts::Longest
                                                         : ScuffParts::Combined;
        (i ? s.pivot_parts : s.slide_parts) = mode;
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
    // marker, colour and on/off; only its kind changes. Story 10-8m: so does a slide / pivot part
    // block switched between Each part and Longest part.
    for (size_t i = 0; i < old.size(); ++i) {
        AutoKind k;
        char     side;
        if (keep[i] || !AutoBlockKind(old[i], &k, &side) || !SwapGroup(k)) continue;
        for (size_t j = 0; j < want.size(); ++j) {
            AutoKind wk;
            char     ws;
            AutoBlockKind(want[j], &wk, &ws);
            if (match_of_want[j] < 0 && SwapGroup(wk) == SwapGroup(k) && ws == side) {
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
    std::vector<bool> rid_used(old.size(), false);
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
        // Decision 2026-10-12 (13-2 option A): it also keeps the dropped block's rule id (its
        // sounds), each dropped block's id going to one new block only.
        nb.rid.clear();
        for (size_t i = 0; i < old.size(); ++i) {
            AutoKind k;
            char     side;
            if (keep[i] || rid_used[i] || old[i].rid.empty() || !AutoBlockKind(old[i], &k, &side) || side != ws ||
                AutoTypeOfKind(k) != AutoTypeOfKind(wk))
                continue;
            rid_used[i] = true;
            nb.rid = old[i].rid;
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
        // Story 10-8m: a slide / pivot switched between combined and separate, as steps.
        else if (k == AutoKind::Slide || k == AutoKind::Pivot) {
            const bool sl = k == AutoKind::Slide;
            map[i] = find_new(sl ? AutoKind::SlideHeel : AutoKind::PivotHeel, side);
            if (map[i] < 0) map[i] = find_new(sl ? AutoKind::SlideHeelLongest : AutoKind::PivotHeelLongest, side);
        } else if (ScuffMode(k) != ScuffParts::Combined)
            map[i] = find_new(AutoTypeOfKind(k) == AutoType::Slide ? AutoKind::Slide : AutoKind::Pivot, side);
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

PhysicsParams AutoPhysicsParams(AutoType t, double sens, bool looping)
{
    PhysicsParams p;
    p.looping = looping;
    const double  f = SensitivityFactor(sens);
    if (!InRange(static_cast<int>(t), kAutoTypeCount)) return p;
    for (const AutoSensField& sf : AutoTypeRow(t).sens) {
        if (!sf.field) continue;
        if (sf.dir == AutoSensDir::Times) p.*sf.field *= f;
        else p.*sf.field /= f;
    }
    return p;
}

bool SameAutoAnalysis(const PhysicsParams& a, const PhysicsParams& b)
{
    // Every constant a type's sensitivity moves, plus the hands' switch cost and looping. A new
    // PhysicsParams constant that changes the analysis but no sensitivity moves must be added to
    // the explicit comparison below.
    for (int t = 0; t < kAutoTypeCount; ++t)
        for (const AutoSensField& sf : AutoTypeRow(static_cast<AutoType>(t)).sens)
            if (sf.field && !(a.*sf.field == b.*sf.field)) return false;
    return a.hand_switch_cost == b.hand_switch_cost && a.looping == b.looping;
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

std::vector<PhysicsParams> AutoParamSets(const std::vector<Block>& blocks, bool looping)
{
    std::vector<PhysicsParams> out;
    for (const Block& b : blocks) {
        AutoKind k;
        if (!b.enabled || !AutoBlockKind(b, &k, nullptr)) continue;
        const PhysicsParams p = AutoPhysicsParams(AutoTypeOfKind(k), b.sens, looping);
        bool                seen = false;
        for (const PhysicsParams& q : out) seen = seen || SameAutoAnalysis(p, q);
        if (!seen) out.push_back(p);
    }
    return out;
}

namespace {

// AutoEvents; `replace` (story 10-9a): Land / Jump events replace the steps / lift-offs near them
// (off for Detect's per-block event counts, which read each block's own detections).
std::vector<Event> AutoEventsImpl(const std::vector<AutoAnalysis>& analyses, const std::vector<Block>& blocks,
                                  bool looping, bool replace)
{
    std::vector<Event> out;
    std::vector<double>   raw;     // per event in `out`: its physics time (before the block's offset)
    std::vector<AutoType> type;    // per event in `out`: its block's type
    std::vector<double>   period;  // per event in `out`: its analysis' loop period (0: not looping)
    // Each analysis' event sources (the kinds' rows: foot events, hand events...), each computed
    // once, when a block first reads it.
    struct Source {
        size_t                    analysis;
        AutoEventSourceFn         fn;
        std::vector<PhysicsEvent> events;
    };
    std::vector<Source> sources;
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        const Block& b = blocks[bi];
        AutoKind     k;
        char         side;
        if (!b.enabled || !AutoBlockKind(b, &k, &side)) continue;
        const PhysicsParams p = AutoPhysicsParams(AutoTypeOfKind(k), b.sens, looping);
        size_t              ai = analyses.size();
        for (size_t j = 0; j < analyses.size() && ai == analyses.size(); ++j)
            if (SameAutoAnalysis(analyses[j].params, p)) ai = j;
        if (ai == analyses.size()) continue;
        const PhysicsAnalysis& a = analyses[ai].analysis;
        if (!a.ok) continue;
        const AutoKindInfo& row = AutoKindRow(k);
        size_t              si = sources.size();
        for (size_t j = 0; j < sources.size() && si == sources.size(); ++j)
            if (sources[j].analysis == ai && sources[j].fn == row.source) si = j;
        if (si == sources.size()) sources.push_back(Source{ai, row.source, row.source(a, analyses[ai].params)});
        for (const PhysicsEvent& e : sources[si].events) {
            if (e.side != side || !row.match(e, a, side)) continue;
            Event ev;
            ev.time_s = e.time_s + b.offset_ms / 1000.0;
            // Story 10-8i: on a loop the offset wraps round the cycle (period T = the seam), as the
            // events themselves do: kept in [0, T).
            if (looping && a.looping && a.window_samples > 1) {
                const double T = static_cast<double>(a.window_samples - 1) / a.rate_hz;
                double       t = std::fmod(ev.time_s, T);
                if (t < 0.0) t += T;
                if (t >= T - 1e-9 || t < 1e-9) t = 0.0;
                ev.time_s = t;
            }
            ev.block = static_cast<int>(bi);
            ev.marker = b.marker;
            ev.strength = e.strength;
            ev.speed = row.degrees ? 0.0 : e.strength * a.leg_length;
            out.push_back(std::move(ev));
            raw.push_back(e.time_s);
            type.push_back(row.type);
            period.push_back(looping && a.looping && a.window_samples > 1
                                 ? static_cast<double>(a.window_samples - 1) / a.rate_hz
                                 : 0.0);
        }
    }
    // Story 10-9a: a Land replaces the steps within kAutoReplaceS of it, a Jump the lift-offs, on
    // their physics times (a block's offset never adds or removes one). Only when both types run.
    if (replace) {
        std::vector<char> drop(out.size(), 0);
        bool              any = false;
        for (size_t i = 0; i < out.size(); ++i) {
            if (type[i] != AutoType::Land && type[i] != AutoType::Jump) continue;
            const AutoType replaced = type[i] == AutoType::Land ? AutoType::Step : AutoType::LiftOff;
            for (size_t j = 0; j < out.size(); ++j) {
                if (type[j] != replaced) continue;
                double d = std::fabs(raw[j] - raw[i]);
                // A loop: the distance through the seam too.
                if (period[i] > 0.0) {
                    d = std::fmod(d, period[i]);
                    d = std::min(d, period[i] - d);
                }
                if (d <= kAutoReplaceS + 1e-9) {
                    drop[j] = 1;
                    any = true;
                }
            }
        }
        if (any) {
            std::vector<Event> kept;
            for (size_t i = 0; i < out.size(); ++i)
                if (!drop[i]) kept.push_back(std::move(out[i]));
            out = std::move(kept);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Event& x, const Event& y) {
        return std::make_tuple(x.time_s, x.block) < std::make_tuple(y.time_s, y.block);
    });
    return out;
}

// DetectAutoEvents; `replace` as AutoEventsImpl.
std::vector<Event> DetectImpl(const std::vector<Block>& blocks, const std::vector<BoneTrack>& role_tracks,
                              std::vector<AutoAnalysis>* cache, bool looping, bool replace)
{
    if (!HasActiveAutoBlocks(blocks)) return {};
    std::vector<AutoAnalysis>  local;
    std::vector<AutoAnalysis>& runs = cache ? *cache : local;
    const std::vector<PhysicsParams> sets = AutoParamSets(blocks, looping);
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
    return AutoEventsImpl(runs, blocks, looping, replace);
}

}  // namespace

std::vector<Event> AutoEvents(const std::vector<AutoAnalysis>& analyses, const std::vector<Block>& blocks, bool looping)
{
    return AutoEventsImpl(analyses, blocks, looping, true);
}

std::vector<Event> DetectAutoEvents(const std::vector<Block>& blocks, const std::vector<BoneTrack>& role_tracks,
                                    std::vector<AutoAnalysis>* cache, bool looping)
{
    return DetectImpl(blocks, role_tracks, cache, looping, true);
}

// ---- Sensitivity search (story 10-8h) ---------------------------------------------------------

namespace {

// Block `b`'s detected events at sensitivity `sens` (-1 = its analysis failed), on `runs`.
int CountAt(const Block& b, double sens, const std::vector<BoneTrack>& role_tracks, std::vector<AutoAnalysis>& runs,
            bool looping)
{
    Block one = b;
    one.sens = sens;
    const std::vector<Block> blocks = {one};
    const std::vector<Event> ev = DetectAutoEvents(blocks, role_tracks, &runs, looping);
    AutoKind k;
    AutoBlockKind(one, &k, nullptr);
    const PhysicsParams p = AutoPhysicsParams(AutoTypeOfKind(k), sens, looping);
    for (const AutoAnalysis& r : runs)
        if (SameAutoAnalysis(r.params, p) && !r.analysis.ok) return -1;
    return static_cast<int>(ev.size());
}

}  // namespace

std::vector<AutoSensResult> SearchAutoSensitivity(const std::vector<Block>& blocks, const std::vector<BoneTrack>& role_tracks,
                                                  const std::vector<int>& event_counts, const std::vector<int>& role_to_bone,
                                                  bool looping)
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
        if (CountAt(b, start, role_tracks, runs, looping) != 0) continue;  // a failed analysis, or events after all
        const int at100 = CountAt(b, 100.0, role_tracks, runs, looping);
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
            const int n = CountAt(b, mid, role_tracks, runs, looping);
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
                         std::vector<AutoAnalysis>* cache, bool looping)
{
    std::vector<AutoSensResult> res;
    bool                        changed = false;
    if (HasActiveAutoBlocks(blocks)) {
        std::vector<int> counts(blocks.size(), 0);
        // Each block's own detections (story 10-9a: before Land / Jump replace steps / lift-offs).
        for (const Event& e : DetectImpl(blocks, role_tracks, cache, looping, false))
            if (e.block >= 0 && e.block < static_cast<int>(counts.size())) ++counts[static_cast<size_t>(e.block)];
        res = SearchAutoSensitivity(blocks, role_tracks, counts, role_to_bone, looping);
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

namespace {

// The body scale: heel, knee and up leg (or the hips) of one side; else knee and up leg of one
// side (twice the thigh). `out` (optional): each side's missing heel, knee and up leg when
// neither works.
bool BodyScale(const std::vector<int>& role_to_bone, std::vector<std::string>* out)
{
    auto       has = [&](Role r) { return AutoHasRole(role_to_bone, r); };
    const Role knee[2] = {Role::LeftKnee, Role::RightKnee};
    const Role up_leg[2] = {Role::LeftUpLeg, Role::RightUpLeg};
    bool       scale = false;
    for (int s = 0; s < 2; ++s)
        scale = scale || (has(FootPartRole(kSides[s], 0)) && has(knee[s]) && (has(up_leg[s]) || has(Role::Hips))) ||
                (has(knee[s]) && has(up_leg[s]));
    if (!scale && out)
        for (int s = 0; s < 2; ++s) {
            if (!has(FootPartRole(kSides[s], 0))) AutoPushOnce(*out, RoleName(FootPartRole(kSides[s], 0)));
            if (!has(knee[s])) AutoPushOnce(*out, RoleName(knee[s]));
            if (!has(up_leg[s]) && !has(Role::Hips)) AutoPushOnce(*out, RoleName(up_leg[s]));
        }
    return scale;
}

}  // namespace

std::vector<std::string> AutoMissingParts(AutoType t, bool separate, const std::vector<int>& role_to_bone)
{
    std::vector<std::string> out;
    BodyScale(role_to_bone, &out);
    if (InRange(static_cast<int>(t), kAutoTypeCount)) AutoTypeRow(t).parts(separate, role_to_bone, out);
    return out;
}

bool AutoBlockPartMissing(const Block& b, const std::vector<int>& role_to_bone)
{
    AutoKind kind;
    char     side = 'L';
    if (!b.enabled || !AutoBlockKind(b, &kind, &side)) return false;
    if (!BodyScale(role_to_bone, nullptr)) return true;
    return AutoKindRow(kind).part_missing(side, role_to_bone);
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

// ---- Signal lanes (story 10-8s) ---------------------------------------------------------------

std::vector<AutoLane> AutoBlockLanes(const AutoAnalysis& aa, const Block& b, std::string* why)
{
    std::vector<AutoLane> out;
    std::string           reason;
    AutoKind              k;
    char                  side;
    if (why) why->clear();
    if (!AutoBlockKind(b, &k, &side)) return out;
    const PhysicsAnalysis& a = aa.analysis;
    if (!a.ok) {
        if (why) *why = a.error.empty() ? std::string("analysis failed") : a.error;
        return out;
    }
    AutoLaneArgs args;
    args.a = &a;
    args.p = &aa.params;
    args.side = side;
    args.first = a.window_first;
    args.n = a.window_samples;
    AutoKindRow(k).lanes(args, out, reason);
    for (AutoLane& l : out) l.rate_hz = a.rate_hz;
    if (out.empty() && why) *why = reason.empty() ? std::string("part missing") : reason;
    return out;
}

}  // namespace rav
