// SPDX-License-Identifier: MIT
//
// See zone_hit.h.

#include "zone_hit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "sound_entries.h"  // PartIndex

namespace rav {
namespace {

double Clamp(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

const EditZone& Z(const SoundData& d, const ZoneRef& r) { return *ZoneOf(d, r); }

bool Valid(const SoundData& d, const ZoneRef& r) { return ZoneOf(d, r) != nullptr; }

int Ms(double t) { return static_cast<int>(std::lround(t * 1000.0)); }

}  // namespace

bool ZoneMine(const SoundData& d, const ZoneRef& r, int lane)
{
    const EditZone* z = ZoneOf(d, r);
    if (!z) return false;
    return lane < 0 || z->IsGap() || PartIndex(d, z->part) == lane;
}

ZoneHit HitZones(const SoundData& d, const ZoneHitView& v, float x, float y)
{
    ZoneHit h;
    h.row = v.row;
    h.lane = v.lane;
    const double vl = v.vl > 0.0 ? v.vl : 1.0;
    const float  w = std::max(1.0f, v.w);
    const double hi_t = v.file_len > 0.0 ? v.file_len : 1e300;
    h.t = Clamp(v.v0 + static_cast<double>(x) / w * vl, 0.0, hi_t);
    h.top = y < v.h * 0.5f;
    auto X = [&](double t) { return static_cast<float>((t - v.v0) / vl * w); };
    const float dp = v.dp > 0.0f ? v.dp : 1.0f;
    if (v.row == ZoneRow::Ruler) {
        if (v.has_tsel) {
            if (std::fabs(x - X(v.ts_a)) <= 5.0f * dp) {
                h.kind = ZoneHitKind::TselEdge;
                h.side = -1;
                return h;
            }
            if (std::fabs(x - X(v.ts_b)) <= 5.0f * dp) {
                h.kind = ZoneHitKind::TselEdge;
                h.side = 1;
                return h;
            }
        }
        h.kind = ZoneHitKind::Ruler;
        return h;
    }
    if (v.row == ZoneRow::Vars) {
        for (const ZoneVarBlock& b : ZoneVarBlocks(d))
            if (h.t >= b.a && h.t < b.b) {
                h.kind = ZoneHitKind::Var;
                h.var = b.frame;
                return h;
            }
        return h;
    }
    if (v.row == ZoneRow::Placed) {
        const ZoneRef r = ZoneAt(d, h.t);
        if (Valid(d, r)) {
            h.kind = ZoneHitKind::Pick;
            h.z = r;
        }
        return h;
    }
    // A lane.
    std::vector<ZoneRef> parts, gaps;
    const double         v1 = v.v0 + vl;
    for (const ZoneRef& r : AllZones(d)) {
        const EditZone& z = Z(d, r);
        if (z.b < v.v0 || z.a > v1) continue;
        if (z.IsGap()) gaps.push_back(r);
        else if (ZoneMine(d, r, v.lane)) parts.push_back(r);
    }
    if (y < 16.0f * dp) {
        for (const ZoneRef& g : gaps) {
            const EditZone& z = Z(d, g);
            if (std::fabs(x - X(z.a + z.xl)) <= 5.0f * dp) {
                h.kind = ZoneHitKind::Xf;
                h.z = g;
                h.side = -1;
                return h;
            }
            if (std::fabs(x - X(z.b - z.xr)) <= 5.0f * dp) {
                h.kind = ZoneHitKind::Xf;
                h.z = g;
                h.side = 1;
                return h;
            }
        }
        for (const ZoneRef& r : parts) {
            const EditZone& z = Z(d, r);
            if (X(z.b) - X(z.a) <= 14.0f * dp) continue;
            if (!Valid(d, GapBefore(d, r)) && std::fabs(x - X(z.a + ZoneFade(d, r, -1).s)) <= 5.0f * dp) {
                h.kind = ZoneHitKind::Fade;
                h.z = r;
                h.side = -1;
                return h;
            }
            if (!Valid(d, GapAfter(d, r)) && std::fabs(x - X(z.b - ZoneFade(d, r, 1).s)) <= 5.0f * dp) {
                h.kind = ZoneHitKind::Fade;
                h.z = r;
                h.side = 1;
                return h;
            }
        }
    }
    if (y >= 24.0f * dp && y <= 42.0f * dp)
        for (const ZoneRef& r : parts)
            if (std::fabs(x - X(ZoneSnap(d, r, v.peaks).t)) <= 6.0f * dp) {
                h.kind = ZoneHitKind::Snap;
                h.z = r;
                return h;
            }
    if (y > v.h - 16.0f * dp) {
        const std::vector<ZoneRef> all = AllZones(d);
        for (size_t i = 0; i + 1 < all.size(); ++i) {
            const EditZone &a = Z(d, all[i]), &b = Z(d, all[i + 1]);
            if (a.IsGap() || b.IsGap() || !Butted(d, all[i], all[i + 1])) continue;
            if (!ZoneMine(d, all[i], v.lane) && !ZoneMine(d, all[i + 1], v.lane)) continue;
            if (std::fabs(x - X(a.b)) <= 5.0f * dp) {
                h.kind = ZoneHitKind::Cut;
                h.z = all[i];
                h.z2 = all[i + 1];
                return h;
            }
        }
    }
    for (const ZoneRef& g : gaps) {
        const EditZone& z = Z(d, g);
        if (std::fabs(x - X(z.a)) <= 4.0f * dp) {
            h.kind = ZoneHitKind::GapEdge;
            h.z = g;
            h.side = -1;
            return h;
        }
        if (std::fabs(x - X(z.b)) <= 4.0f * dp) {
            h.kind = ZoneHitKind::GapEdge;
            h.z = g;
            h.side = 1;
            return h;
        }
    }
    struct Cand {
        ZoneRef r;
        int     side;
        float   dist;
    };
    std::vector<Cand> cands;
    for (const ZoneRef& r : parts) {
        const EditZone& z = Z(d, r);
        if (!Valid(d, GapBefore(d, r)) && std::fabs(x - X(z.a)) <= 5.0f * dp) cands.push_back({r, -1, std::fabs(x - X(z.a))});
        if (!Valid(d, GapAfter(d, r)) && std::fabs(x - X(z.b)) <= 5.0f * dp) cands.push_back({r, 1, std::fabs(x - X(z.b))});
    }
    if (!cands.empty()) {
        const Cand* pick = nullptr;
        for (const Cand& c : cands)
            if (h.t >= Z(d, c.r).a && h.t < Z(d, c.r).b) {
                pick = &c;
                break;
            }
        if (!pick) {
            pick = &cands.front();
            for (const Cand& c : cands)
                if (c.dist < pick->dist) pick = &c;
        }
        h.kind = ZoneHitKind::Edge;
        h.z = pick->r;
        h.side = pick->side;
        return h;
    }
    const ZoneRef r = ZoneAt(d, h.t);
    if (Valid(d, r)) {
        h.z = r;
        h.kind = ZoneMine(d, r, v.lane) ? ZoneHitKind::Zone : ZoneHitKind::Grey;
    }
    return h;
}

ZoneDragKind PressDragKind(const ZoneHit& h, bool ctrl, bool alt)
{
    switch (h.kind) {
    case ZoneHitKind::TselEdge: return ZoneDragKind::TselEdge;
    case ZoneHitKind::Xf: return ctrl ? ZoneDragKind::Reserved : ZoneDragKind::Xf;
    case ZoneHitKind::Edge: return ctrl || alt ? ZoneDragKind::Reserved : ZoneDragKind::Edge;
    case ZoneHitKind::Cut: return ctrl || alt ? ZoneDragKind::Reserved : ZoneDragKind::Cut;
    case ZoneHitKind::GapEdge: return ctrl || alt ? ZoneDragKind::Reserved : ZoneDragKind::GapEdge;
    case ZoneHitKind::Fade: return ctrl || alt ? ZoneDragKind::Reserved : ZoneDragKind::Fade;
    case ZoneHitKind::Snap: return ctrl || alt ? ZoneDragKind::Reserved : ZoneDragKind::Snap;
    default: return ZoneDragKind::Pending;
    }
}

ZoneDragKind StartZoneDrag(const ZoneHit& h, bool shift, bool alt)
{
    if (h.row == ZoneRow::Vars || h.row == ZoneRow::Placed) return ZoneDragKind::None;
    if (h.row == ZoneRow::Ruler) return ZoneDragKind::Tsel;
    if (alt) return ZoneDragKind::MkGap;
    if (shift) return ZoneDragKind::MkZone;
    if (h.kind == ZoneHitKind::Grey) return ZoneDragKind::Swipe;
    if (h.kind == ZoneHitKind::Zone && h.top) return ZoneDragKind::Move;
    return ZoneDragKind::Scrub;
}

bool PendingBecomesGain(const SoundData& d, const ZoneHit& h, float dx, float dy, bool shift, bool alt)
{
    if (h.kind != ZoneHitKind::Zone || h.row != ZoneRow::Lane || shift || alt) return false;
    const EditZone* z = ZoneOf(d, h.z);
    if (!z || z->IsGap()) return false;
    return std::fabs(dy) > 3.0f && std::fabs(dy) > std::fabs(dx);
}

double GainDragDb(float dy_px)
{
    return std::round(static_cast<double>(-dy_px)) / 10.0;
}

std::string NewZonePart(const SoundData& d, const ZoneHit& h, const std::string& part_sel)
{
    if (d.divide_values.empty()) return {};
    if (h.lane >= 0 && static_cast<size_t>(h.lane) < d.divide_values.size()) return d.divide_values[static_cast<size_t>(h.lane)];
    if (PartIndex(d, part_sel) >= 0) return part_sel;
    const EditZone* z = ZoneOf(d, ZoneAt(d, h.t));
    if (z && !z->IsGap()) return z->part;
    return d.divide_values.front();
}

double SnapZoneTime(const SoundData& d, double t, double px_per_s, double cursor, float tol_px)
{
    double best = t, bd = tol_px;
    auto   try_c = [&](double c) {
        const double dist = std::fabs(c - t) * px_per_s;
        if (dist < bd) {
            bd = dist;
            best = c;
        }
    };
    for (const ZoneRef& r : AllZones(d)) {
        try_c(Z(d, r).a);
        try_c(Z(d, r).b);
    }
    if (cursor >= 0.0) try_c(cursor);
    return best;
}

std::string ZoneTapKey(const ZoneHit& h)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%d|%d|%d|%d.%d|%d.%d|%d|%d", static_cast<int>(h.row), h.lane, static_cast<int>(h.kind),
                  h.z.frame, h.z.index, h.z2.frame, h.z2.index, h.var, h.side);
    return buf;
}

bool ZoneDoubleTap(ZoneTap& last, const std::string& key, double now, float x, float y, float tol_px, double max_s)
{
    const bool dbl = last.valid && last.key == key && now - last.time < max_s && std::hypot(x - last.x, y - last.y) < tol_px;
    if (dbl) {
        last = ZoneTap{};
    } else {
        last.valid = true;
        last.key = key;
        last.time = now;
        last.x = x;
        last.y = y;
    }
    return dbl;
}

ZoneDoubleAction DoubleActionOf(const ZoneHit& h)
{
    switch (h.kind) {
    case ZoneHitKind::Ruler:
    case ZoneHitKind::None: return ZoneDoubleAction::RemoveTsel;
    case ZoneHitKind::Var: return ZoneDoubleAction::TselVariation;
    case ZoneHitKind::Pick:
    case ZoneHitKind::Zone:
    case ZoneHitKind::Grey: return ZoneDoubleAction::TselZone;
    case ZoneHitKind::Edge: return ZoneDoubleAction::ResetEdge;
    case ZoneHitKind::Snap: return ZoneDoubleAction::ResetSnap;
    case ZoneHitKind::Fade: return ZoneDoubleAction::ResetFade;
    default: return ZoneDoubleAction::None;
    }
}

ZoneCursor ZoneCursorOf(const ZoneHit& h)
{
    switch (h.kind) {
    case ZoneHitKind::TselEdge:
    case ZoneHitKind::Snap:
    case ZoneHitKind::Xf:
    case ZoneHitKind::Fade:
    case ZoneHitKind::Cut:
    case ZoneHitKind::GapEdge:
    case ZoneHitKind::Edge: return ZoneCursor::ResizeEW;
    case ZoneHitKind::Var:
    case ZoneHitKind::Grey:
    case ZoneHitKind::Pick: return ZoneCursor::Hand;
    case ZoneHitKind::Zone: return h.top ? ZoneCursor::Grab : ZoneCursor::Text;
    case ZoneHitKind::Ruler:
    case ZoneHitKind::None: return ZoneCursor::Text;
    }
    return ZoneCursor::Arrow;
}

std::string ZoneName(const SoundData& d, const ZoneRef& r, const std::vector<std::string>& names)
{
    const EditZone* z = ZoneOf(d, r);
    if (!z) return {};
    if (z->IsGap()) return "Gap";
    const int k = PartIndex(d, z->part);
    return k >= 0 && static_cast<size_t>(k) < names.size() ? names[static_cast<size_t>(k)] : std::string("?");
}

std::string ZoneTip(const SoundData& d, const ZoneHit& h, const std::vector<std::string>& names, bool click_plays)
{
    const std::string n = ZoneName(d, h.z, names);
    const std::string lane = h.lane >= 0 && static_cast<size_t>(h.lane) < names.size() ? names[static_cast<size_t>(h.lane)] : std::string();
    const EditZone*   z = ZoneOf(d, h.z);
    const std::string plays = click_plays ? " and play" : "";
    switch (h.kind) {
    case ZoneHitKind::Ruler:
        return "Click: the edit cursor. Double-click: removes the time selection. Drag: a time selection (playback stays "
               "inside it; Loop repeats it)";
    case ZoneHitKind::TselEdge: return "Time selection edge: drag it";
    case ZoneHitKind::Var:
        return "Variation " + std::to_string(h.var + 1) +
               ": click selects all its zones; double-click: a time selection on it, the cursor at its start";
    case ZoneHitKind::Pick: return n + ": click to select; double-click: a time selection on it, the cursor at its start";
    case ZoneHitKind::Xf: return "Crossfade of the Gap: drag (both sides together; Alt: this side only)";
    case ZoneHitKind::Fade: return n + " fade " + (h.side < 0 ? "in" : "out") + ": drag (double-click: its default)";
    case ZoneHitKind::Snap: return n + " snap offset (where the event lands): drag (double-click: automatic)";
    case ZoneHitKind::Cut: return "Cut between two zones: drag to move it (both edges together)";
    case ZoneHitKind::GapEdge: return "Gap edge: drag (the zone beside it follows)";
    case ZoneHitKind::Edge:
        return n + " " + (h.side < 0 ? "start" : "end") + ": drag (double-click: automatic, follows " +
               (h.side < 0 ? "Pre-roll" : "Tail") + ")";
    case ZoneHitKind::Zone:
        if (z && z->IsGap()) return "Gap: drag to move it (its neighbours follow). Click: select";
        return h.top ? n + ": drag up / down: its gain. Drag sideways to move it (its two edges; what touches it follows). Click: select" + plays
                     : n + ": drag up / down: its gain. Drag sideways to move the cursor. Click: select" + plays;
    case ZoneHitKind::Grey:
        return "Played by " + n + ": click to give it to " + lane + "; drag: swipe a range to " + lane;
    case ZoneHitKind::None: break;
    }
    if (h.row == ZoneRow::Vars || h.row == ZoneRow::Placed) return {};
    return "Nothing placed here. Shift+drag: a new zone. Alt+drag: a Gap. Drag: the cursor. Double-click: removes the time "
           "selection";
}

std::vector<ZoneVarBlock> ZoneVarBlocks(const SoundData& d)
{
    std::vector<ZoneVarBlock> out;
    for (size_t f = 0; f < d.frames.size(); ++f) {
        const SoundFrame& fr = d.frames[f];
        if (!fr.on || fr.zones.empty()) continue;
        ZoneVarBlock b;
        b.frame = static_cast<int>(f);
        if (!VariationEdges(d, b.frame, &b.a, &b.b) || !(b.b > b.a)) continue;
        out.push_back(b);
    }
    std::stable_sort(out.begin(), out.end(), [](const ZoneVarBlock& x, const ZoneVarBlock& y) { return x.a < y.a; });
    return out;
}

std::string ZoneVarLabel(int n, double a, double b)
{
    return std::to_string(n) + " \xC2\xB7 " + ZoneRangeMs(a, b);
}

ZoneSel ZonesOfFrame(const SoundData& d, int frame)
{
    ZoneSel out;
    for (const ZoneRef& r : AllZones(d))
        if (r.frame == frame) out.push_back(r);
    return out;
}

ZoneRef FirstZoneOf(const SoundData& d, int frame)
{
    const ZoneSel all = ZonesOfFrame(d, frame);
    for (const ZoneRef& r : all)
        if (!Z(d, r).IsGap()) return r;
    return all.empty() ? ZoneRef{} : all.front();
}

std::vector<int> SelFrames(const ZoneSel& sel)
{
    std::set<int> s;
    for (const ZoneRef& r : sel)
        if (r.frame >= 0) s.insert(r.frame);
    return std::vector<int>(s.begin(), s.end());
}

bool SelHas(const ZoneSel& sel, const ZoneRef& r)
{
    return std::find(sel.begin(), sel.end(), r) != sel.end();
}

void SelToggle(ZoneSel& sel, const ZoneRef& r)
{
    const auto it = std::find(sel.begin(), sel.end(), r);
    if (it != sel.end()) sel.erase(it);
    else sel.push_back(r);
}

ZoneSel SelValid(const SoundData& d, const ZoneSel& sel)
{
    ZoneSel out;
    for (const ZoneRef& r : sel)
        if (Valid(d, r) && !SelHas(out, r)) out.push_back(r);
    return out;
}

ZoneSel SelFollow(const SoundData& before, const ZoneSel& sel, const SoundData& after)
{
    ZoneSel out;
    const std::vector<ZoneRef> all = AllZones(after);
    for (const ZoneRef& r : sel) {
        const EditZone* z = ZoneOf(before, r);
        if (!z) continue;
        auto same = [&](const EditZone& y) { return y.IsGap() == z->IsGap() && y.part == z->part; };
        auto overlap = [&](const EditZone& y) { return std::min(y.b, z->b) - std::max(y.a, z->a); };
        const EditZone* here = ZoneOf(after, r);
        if (here && same(*here) && overlap(*here) > 0.0) {
            if (!SelHas(out, r)) out.push_back(r);
            continue;
        }
        ZoneRef best;
        double  bo = 0.0;
        for (const ZoneRef& q : all) {
            const EditZone& y = Z(after, q);
            if (!same(y)) continue;
            const double o = overlap(y);
            if (o > bo) {
                bo = o;
                best = q;
            }
        }
        if (Valid(after, best) && !SelHas(out, best)) out.push_back(best);
    }
    return out;
}

ZoneSel SelSorted(const SoundData& d, const ZoneSel& sel)
{
    ZoneSel out;
    for (const ZoneRef& r : AllZones(d))
        if (SelHas(sel, r)) out.push_back(r);
    return out;
}

std::string ZoneRangeMs(double a, double b)
{
    return std::to_string(Ms(a)) + "\xE2\x80\x93" + std::to_string(Ms(b)) + " ms";
}

std::string ZoneDbText(double db)
{
    char buf[32];
    const double r = std::round(db * 10.0) / 10.0;
    std::snprintf(buf, sizeof(buf), "%s%.1f dB", r > 0.0 ? "+" : "", r == 0.0 ? 0.0 : r);
    return buf;
}

}  // namespace rav
