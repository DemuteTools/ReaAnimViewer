// SPDX-License-Identifier: MIT
//
// See event_list.h.

#include "event_list.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rav {
namespace {

bool KnownKind(EventKind k)
{
    return k == EventKind::Detected || k == EventKind::User || k == EventKind::Suppress;
}

bool Within(double a, double b, double w)
{
    return std::fabs(a - b) <= w + 1e-9;
}

// The marker name on one line, trimmed (as the record writes it).
std::string OneLine(const std::string& s)
{
    std::string t = s;
    for (char& c : t)
        if (c == '\r' || c == '\n') c = ' ';
    const size_t b = t.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    const size_t e = t.find_last_not_of(' ');
    return t.substr(b, e - b + 1);
}

double SafeRate(const ItemClipMap& m)
{
    return (m.rate > 0.0 && std::isfinite(m.rate)) ? m.rate : 1.0;
}

}  // namespace

std::vector<ShownEvent> BuildEventList(const std::vector<Event>& detections, const std::vector<EventEntry>& entries,
                                       size_t n_blocks)
{
    std::vector<ShownEvent> out;
    const int nb = static_cast<int>(n_blocks);
    auto in_range = [&](int b) { return b >= 0 && b < nb; };
    std::vector<char> masking(entries.size(), 0);  // a suppression that masks a detection
    for (size_t d = 0; d < detections.size(); ++d) {
        const Event& e = detections[d];
        if (!in_range(e.block)) continue;
        ShownEvent s;
        s.t = e.time_s;
        s.block = e.block;
        s.strength = e.strength;
        s.speed = e.speed;
        s.detection = static_cast<int>(d);
        s.kind = ShownKind::Detected;
        // The nearest suppression of this rule within the window.
        double best = 1e300;
        for (size_t i = 0; i < entries.size(); ++i) {
            const EventEntry& x = entries[i];
            if (x.kind != EventKind::Suppress || x.block != e.block || !Within(x.t, e.time_s, kSuppressWindowS)) continue;
            const double dist = std::fabs(x.t - e.time_s);
            if (dist < best) {
                best = dist;
                s.kind = ShownKind::Suppressed;
                s.entry = static_cast<int>(i);
            }
        }
        if (s.kind == ShownKind::Suppressed) masking[static_cast<size_t>(s.entry)] = 1;
        out.push_back(s);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        const EventEntry& x = entries[i];
        if (!in_range(x.block)) continue;
        if (x.kind == EventKind::Suppress) {
            // An orphan: nothing of its rule detected within the window now.
            bool near = masking[i] != 0;
            for (const Event& e : detections)
                if (!near && e.block == x.block && Within(x.t, e.time_s, kSuppressWindowS)) near = true;
            if (near) continue;
            ShownEvent s;
            s.t = x.t;
            s.block = x.block;
            s.kind = ShownKind::Orphan;
            s.entry = static_cast<int>(i);
            out.push_back(s);
        } else if (x.kind == EventKind::User) {
            ShownEvent s;
            s.t = x.t;
            s.block = x.block;
            s.kind = ShownKind::User;
            s.strength = x.strength;
            s.speed = x.speed;
            s.entry = static_cast<int>(i);
            out.push_back(s);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const ShownEvent& a, const ShownEvent& b) {
        if (a.t != b.t) return a.t < b.t;
        return a.block < b.block;
    });
    return out;
}

EventEntry MakeUserEvent(int block, double t, double strength, double speed)
{
    EventEntry e;
    e.t = t;
    e.kind = EventKind::User;
    e.block = block;
    e.strength = strength;
    e.speed = speed;
    e.has_block = e.has_strength = e.has_speed = true;
    return e;
}

EventEntry MakeSuppression(int block, double t)
{
    EventEntry e;
    e.t = t;
    e.kind = EventKind::Suppress;
    e.block = block;
    e.has_block = true;
    e.has_strength = e.has_speed = false;
    return e;
}

std::vector<int> BlockMapForDelete(size_t n_blocks, int deleted)
{
    std::vector<int> m(n_blocks);
    for (size_t i = 0; i < n_blocks; ++i) {
        const int b = static_cast<int>(i);
        m[i] = b < deleted ? b : (b == deleted ? -1 : b - 1);
    }
    return m;
}

std::vector<int> BlockMapForInsert(size_t n_blocks, int at)
{
    std::vector<int> m(n_blocks);
    for (size_t i = 0; i < n_blocks; ++i) {
        const int b = static_cast<int>(i);
        m[i] = b < at ? b : b + 1;
    }
    return m;
}

void RemapEventBlocks(std::vector<EventEntry>& entries, const std::vector<int>& old_to_new)
{
    std::vector<EventEntry> out;
    out.reserve(entries.size());
    for (EventEntry& e : entries) {
        if (KnownKind(e.kind) && e.block >= 0 && e.block < static_cast<int>(old_to_new.size())) {
            const int nb = old_to_new[static_cast<size_t>(e.block)];
            if (nb < 0) continue;  // its rule is gone: it goes too
            if (nb != e.block) {
                e.block = nb;
                e.has_block = true;
            }
        }
        out.push_back(std::move(e));
    }
    entries = std::move(out);
}

bool FirstClipPass(const ItemClipMap& m, ClipPass* out)
{
    if (!(m.item_len > 0.0) || !(m.clip_len > 0.0) || !std::isfinite(m.item_pos) || !std::isfinite(m.item_len) ||
        !std::isfinite(m.clip_len))
        return false;
    const double rate = SafeRate(m);
    const double offs = std::isfinite(m.start_offs) ? m.start_offs : 0.0;
    const double item_end = m.item_pos + m.item_len;
    auto p_of = [&](double s) { return m.item_pos + (s - offs) / rate; };
    const double a = std::max(m.item_pos, p_of(0.0));
    const double b = std::min(item_end, p_of(m.clip_len));
    if (!(b > a)) return false;
    if (out) *out = ClipPass{a, b, offs + (a - m.item_pos) * rate};
    return true;
}

bool FirstPassProjectTime(const ItemClipMap& m, double clip_t, double* project_t)
{
    ClipPass p;
    if (!std::isfinite(clip_t) || !FirstClipPass(m, &p)) return false;
    const double t = p.p0 + (clip_t - p.clip0) / SafeRate(m);
    if (t < p.p0 - 1e-9 || t > p.p1 + 1e-9) return false;
    if (project_t) *project_t = t;
    return true;
}

bool FirstPassClipTime(const ItemClipMap& m, double project_t, double* clip_t)
{
    ClipPass p;
    if (!std::isfinite(project_t) || !FirstClipPass(m, &p)) return false;
    if (project_t < p.p0 - 1e-9 || project_t > p.p1 + 1e-9) return false;
    if (clip_t) *clip_t = p.clip0 + (project_t - p.p0) * SafeRate(m);
    return true;
}

std::vector<PlannedMarker> PlanMarkers(const std::vector<ShownEvent>& list, const std::vector<Block>& blocks,
                                       const ItemClipMap& map)
{
    std::vector<PlannedMarker> out;
    for (const ShownEvent& e : list) {
        if (e.kind != ShownKind::Detected && e.kind != ShownKind::User) continue;
        if (e.block < 0 || e.block >= static_cast<int>(blocks.size())) continue;
        const Block& b = blocks[static_cast<size_t>(e.block)];
        if (!b.enabled) continue;
        double pt = 0.0;
        if (!FirstPassProjectTime(map, e.t, &pt)) continue;
        PlannedMarker m;
        m.clip_t = e.t;
        m.project_t = pt;
        m.block = e.block;
        m.name = OneLine(b.marker);
        m.color = b.color;
        m.user = e.kind == ShownKind::User;
        m.strength = e.strength;
        m.speed = e.speed;
        out.push_back(m);
    }
    std::stable_sort(out.begin(), out.end(), [](const PlannedMarker& a, const PlannedMarker& b) {
        if (a.clip_t != b.clip_t) return a.clip_t < b.clip_t;
        return a.block < b.block;
    });
    return out;
}

std::string MarkerSignature(const std::vector<PlannedMarker>& planned, MarkerMode mode)
{
    std::string text = MarkerModeWord(mode);
    const bool project = mode != MarkerMode::Take;
    char buf[160];
    for (const PlannedMarker& m : planned) {
        std::snprintf(buf, sizeof(buf), "\n%d|%08X|%.6f|%.6f|", m.block, static_cast<unsigned>(m.color), m.clip_t,
                      project ? m.project_t : 0.0);
        text += buf;
        text += m.name;
    }
    uint64_t h = 1469598103934665603ull;  // FNV-1a 64
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(h));
    return buf;
}

bool MarkersUpToDate(const ItemRules& rules, const std::vector<PlannedMarker>& planned, MarkerMode mode)
{
    return rules.has_applied && rules.applied.mode == mode && rules.applied.sig == MarkerSignature(planned, mode);
}

void RecordApplied(ItemRules& rules, const std::vector<PlannedMarker>& planned, MarkerMode mode)
{
    std::vector<EventEntry> snapshot;
    for (const PlannedMarker& m : planned) {
        if (m.user) continue;
        EventEntry e;
        e.t = m.clip_t;
        e.kind = EventKind::Detected;
        e.block = m.block;
        e.strength = m.strength;
        e.speed = m.speed;
        e.has_block = e.has_strength = e.has_speed = true;
        snapshot.push_back(e);
    }
    rules.events.erase(std::remove_if(rules.events.begin(), rules.events.end(),
                                      [](const EventEntry& e) { return e.kind == EventKind::Detected; }),
                       rules.events.end());
    rules.events.insert(rules.events.begin(), snapshot.begin(), snapshot.end());
    rules.has_applied = true;
    rules.applied.mode = mode;
    rules.applied.sig = MarkerSignature(planned, mode);
}

int FindTwinMarker(const std::vector<ExistingMarker>& existing, double t, const std::string& name, double tol)
{
    for (size_t i = 0; i < existing.size(); ++i)
        if (existing[i].name == name && std::fabs(existing[i].t - t) <= tol + 1e-12) return static_cast<int>(i);
    return -1;
}

void ClearCorrections(ItemRules& rules)
{
    rules.events.erase(std::remove_if(rules.events.begin(), rules.events.end(),
                                      [](const EventEntry& e) { return KnownKind(e.kind); }),
                       rules.events.end());
    rules.has_applied = false;
    rules.applied = AppliedInfo{};
}

}  // namespace rav
