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
        double     pt = 0.0;
        const bool in = FirstPassProjectTime(map, e.t, &pt);
        if (!in) pt = map.item_pos + (e.t - (std::isfinite(map.start_offs) ? map.start_offs : 0.0)) / SafeRate(map);
        PlannedMarker m;
        m.clip_t = e.t;
        m.project_t = pt;
        m.in_item = in;
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
    char        buf[160];
    for (const PlannedMarker& m : planned) {
        std::snprintf(buf, sizeof(buf), "\n%d|%08X|%.6f|", m.block, static_cast<unsigned>(m.color), m.clip_t);
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

namespace {

// The signature as written before 10-4b (the markers inside the item, their project times when
// project markers are written), so a record committed then still reads "up to date" until its
// item moves, as it did.
std::string LegacyMarkerSignature(const std::vector<PlannedMarker>& planned, MarkerMode mode)
{
    std::string text = MarkerModeWord(mode);
    const bool  project = mode != MarkerMode::Take;
    char        buf[160];
    for (const PlannedMarker& m : planned) {
        if (!m.in_item) continue;
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

}  // namespace

bool MarkersUpToDate(const ItemRules& rules, const std::vector<PlannedMarker>& planned, MarkerMode mode)
{
    if (!rules.has_applied || rules.applied.mode != mode) return false;
    return rules.applied.sig == MarkerSignature(planned, mode) || rules.applied.sig == LegacyMarkerSignature(planned, mode);
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

std::vector<MarkerWrite> PlanMarkerWrites(const std::vector<PlannedMarker>& plan, bool want_take, bool want_project)
{
    std::vector<MarkerWrite>    out;
    std::vector<ExistingMarker> hidden;  // (clip time, name) recorded hidden already
    for (size_t i = 0; i < plan.size(); ++i) {
        const PlannedMarker& m = plan[i];
        MarkerWrite          w;
        w.planned = i;
        w.ref.t = m.project_t;  // outside the item: an extrapolation, never placed
        w.ref.c = m.clip_t;
        w.ref.color = m.color;
        w.ref.name = m.name;
        w.ref.has_c = w.ref.has_color = w.ref.has_name = true;
        // Take markers live in source time: one outside the item is hidden by REAPER and shows
        // again when the item is extended, so every planned marker gets one.
        w.take = want_take;
        if (m.in_item) {
            w.project = want_project;
        } else if (want_project && FindTwinMarker(hidden, m.clip_t, m.name) < 0) {
            w.hidden = true;
            hidden.push_back({m.clip_t, m.name});
        }
        if (w.take || w.project || w.hidden) out.push_back(w);
    }
    return out;
}

int FindTwinMarker(const std::vector<ExistingMarker>& existing, double t, const std::string& name, double tol)
{
    for (size_t i = 0; i < existing.size(); ++i)
        if (existing[i].name == name && std::fabs(existing[i].t - t) <= tol + 1e-12) return static_cast<int>(i);
    return -1;
}

std::string PreviewMarkerName(const std::string& name)
{
    return name + kPreviewSuffix;
}

uint32_t PreviewColor(uint32_t color)
{
    if (!(color & 0x1000000u)) return 0x1000000u | 0x505050u;
    auto half = [](uint32_t c) { return (c + 1u) / 2u; };  // ~50 % toward black, rounded
    const uint32_t r = half((color >> 16) & 0xFFu), g = half((color >> 8) & 0xFFu), b = half(color & 0xFFu);
    return 0x1000000u | (r << 16) | (g << 8) | b;
}

std::vector<PlannedMarker> PreviewPlan(const std::vector<PlannedMarker>& planned)
{
    std::vector<PlannedMarker> out = planned;
    for (PlannedMarker& m : out) {
        m.name = PreviewMarkerName(m.name);
        m.color = PreviewColor(m.color);
    }
    return out;
}

bool PreviewNeeded(const ItemRules& rules, const std::vector<PlannedMarker>& planned, MarkerMode mode)
{
    if (!rules.has_applied) return !planned.empty();
    return !MarkersUpToDate(rules, planned, mode);
}

void RecordPreviewed(ItemRules& rules, const std::vector<PlannedMarker>& planned, MarkerMode mode)
{
    rules.has_previewed = true;
    rules.previewed.mode = mode;
    rules.previewed.sig = MarkerSignature(planned, mode);
}

void ClearPreviewed(ItemRules& rules)
{
    rules.has_previewed = false;
    rules.previewed = AppliedInfo{};
    rules.ptmarkers.clear();
    rules.ppmarkers.clear();
}

bool HasPreviews(const ItemRules& rules)
{
    return rules.has_previewed || !rules.ptmarkers.empty() || !rules.ppmarkers.empty();
}

bool HasRulesForSnapshot(const ItemRules& rules)
{
    return !rules.blocks.empty() || rules.has_preset;
}

ItemRules RestoreCommitted(const ItemRules& snapshot, const ItemRules& current)
{
    ItemRules out = snapshot;
    out.tmarkers = current.tmarkers;
    out.pmarkers = current.pmarkers;
    ClearPreviewed(out);
    // 10-4b: the markers are the current item's (a copy's snapshot names the original).
    const std::string owner = RecordOwner(current);
    if (!owner.empty()) SetRecordOwner(out, owner);
    return out;
}

bool CancelTarget(const ItemRules& cur, const std::string& cur_raw, const std::string* snapshot_text, ItemRules* next)
{
    ItemRules out;
    ItemRules snap;
    if (snapshot_text && !snapshot_text->empty() && ParseItemRules(*snapshot_text, &snap)) {
        out = RestoreCommitted(snap, cur);
    } else {
        out = cur;  // no snapshot: only the previews go
        ClearPreviewed(out);
    }
    if (!HasPreviews(cur) && SerializeItemRules(out) == cur_raw) return false;
    if (next) *next = std::move(out);
    return true;
}

std::string FirstCommittedSnapshot(bool has_snapshot, bool before_valid, const ItemRules& before,
                                   const std::string& before_raw, const ItemRules& after)
{
    if (has_snapshot) return "";
    if (before_valid && HasRulesForSnapshot(before)) return before_raw;
    if (HasRulesForSnapshot(after)) return SerializeItemRules(after);
    return "";
}

void ComposeCommittedRecord(ItemRules& rec, const std::vector<TakeMarkerRef>& own_take,
                            const std::vector<ProjectMarkerRef>& own_project, bool keep_take_refs,
                            bool keep_project_refs, const std::vector<PlannedMarker>& planned, MarkerMode mode)
{
    const std::vector<ProjectMarkerRef> pkeep = keep_project_refs ? rec.ppmarkers : std::vector<ProjectMarkerRef>{};
    const std::vector<TakeMarkerRef>    tkeep = keep_take_refs ? rec.ptmarkers : std::vector<TakeMarkerRef>{};
    if (keep_take_refs) {
        for (const TakeMarkerRef& t : own_take) rec.tmarkers.push_back(t);
    } else {
        rec.tmarkers = own_take;
    }
    if (keep_project_refs) {
        for (const ProjectMarkerRef& p : own_project) rec.pmarkers.push_back(p);
    } else {
        rec.pmarkers = own_project;
    }
    RecordApplied(rec, planned, mode);
    ClearPreviewed(rec);
    rec.ptmarkers = tkeep;  // previews that could not be deleted stay RAV's
    rec.ppmarkers = pkeep;
}

std::vector<MirrorStep> MirrorPlan(const std::vector<ProjectMarkerRef>& refs, const std::vector<MirrorRefState>& state,
                                   const ItemClipMap& map, bool map_changed, bool copy)
{
    std::vector<MirrorStep> out;
    out.reserve(refs.size());
    for (size_t i = 0; i < refs.size(); ++i) {
        const ProjectMarkerRef& r = refs[i];
        const MirrorRefState    st = i < state.size() ? state[i] : MirrorRefState{};
        MirrorStep              s;
        s.ref = i;
        out.push_back(s);
        MirrorStep& step = out.back();
        if (!r.has_c) {
            // Older ref: never moved. On a copy its marker is the original's: not the copy's.
            if (copy && !r.guid.empty()) step.action = MirrorAction::Hide;
            continue;
        }
        double     pt = 0.0;
        const bool in = FirstPassProjectTime(map, r.c, &pt);
        if (copy) {
            // A copy: never touches the original's markers.
            if (in) {
                step.action = MirrorAction::Show;
                step.new_t = pt;
            } else if (!r.guid.empty()) {
                step.action = MirrorAction::Hide;
            }
            continue;
        }
        if (r.guid.empty()) {  // hidden: back when its event is inside the item again
            if (in) {
                step.action = MirrorAction::Show;
                step.new_t = pt;
            }
            continue;
        }
        if (!st.exists) {
            // Deleted by the mirror (hidden, or its item was deleted then restored): back.
            // Deleted by the user: left deleted (10-4c will make that a suppression).
            if (!st.restore) continue;
            if (in) {
                step.action = MirrorAction::Show;
                step.new_t = pt;
            } else {
                step.action = MirrorAction::Hide;
            }
            continue;
        }
        if (!map_changed) continue;  // the item stayed put: a dragged marker is the user's
        if (!in) {
            step.action = MirrorAction::Hide;
            step.delete_old = true;
        } else if (std::fabs(st.now_t - pt) > 1e-9) {
            step.action = MirrorAction::Move;
            step.new_t = pt;
        }
    }
    return out;
}

bool MirrorManaged(const ItemRules& rules)
{
    for (const ProjectMarkerRef& r : rules.pmarkers)
        if (r.has_c) return true;
    for (const ProjectMarkerRef& r : rules.ppmarkers)
        if (r.has_c) return true;
    return false;
}

std::string RecordOwner(const ItemRules& rules)
{
    if (rules.has_applied && !rules.applied.item.empty()) return rules.applied.item;
    if (rules.has_previewed && !rules.previewed.item.empty()) return rules.previewed.item;
    return "";
}

bool SetRecordOwner(ItemRules& rules, const std::string& item_guid)
{
    if (rules.has_applied) rules.applied.item = item_guid;
    if (rules.has_previewed) rules.previewed.item = item_guid;
    return rules.has_applied || rules.has_previewed;
}

MirrorOwnership DecideMirrorOwnership(const std::string& owner, const std::string& item_guid, bool claimed_elsewhere)
{
    if (!owner.empty()) return owner == item_guid ? MirrorOwnership::Own : MirrorOwnership::Copy;
    return claimed_elsewhere ? MirrorOwnership::Copy : MirrorOwnership::Adopt;
}

MirrorOwnerResult ArbitrateMirrorOwnership(const std::vector<MirrorOwnerInput>& items,
                                           const std::map<std::string, std::string>& prev_owners,
                                           const std::vector<std::string>& present)
{
    MirrorOwnerResult res;
    res.own.assign(items.size(), MirrorOwnership::Own);
    auto prev_of = [&](const std::string& g) {
        const auto f = prev_owners.find(g);
        return f == prev_owners.end() ? std::string() : f->second;
    };
    auto self_owner_read = [&](const std::string& item) {
        for (const MirrorOwnerInput& o : items)
            if (o.item == item && o.managed && o.owner == item) return true;
        return false;
    };
    std::vector<std::string> pres = present;
    std::sort(pres.begin(), pres.end());
    // Self-owners first.
    for (size_t i = 0; i < items.size(); ++i) {
        const MirrorOwnerInput& it = items[i];
        if (!it.managed || it.owner != it.item) continue;
        bool lost = false;
        for (const std::string& g : it.guids) {
            const std::string was = prev_of(g);
            if (!was.empty() && was != it.item && self_owner_read(was)) lost = true;
            const auto c = res.claimed.find(g);
            if (c != res.claimed.end() && c->second != it.item) lost = true;
        }
        res.own[i] = lost ? MirrorOwnership::Copy : MirrorOwnership::Own;
        if (!lost)
            for (const std::string& g : it.guids) res.claimed.emplace(g, it.item);
    }
    // Then the others.
    for (size_t i = 0; i < items.size(); ++i) {
        const MirrorOwnerInput& it = items[i];
        if (!it.managed || it.owner == it.item) continue;
        bool elsewhere = false;
        for (const std::string& g : it.guids) {
            const auto c = res.claimed.find(g);
            if (c != res.claimed.end() && c->second != it.item) elsewhere = true;
            const std::string was = prev_of(g);
            if (!was.empty() && was != it.item && std::binary_search(pres.begin(), pres.end(), was)) elsewhere = true;
        }
        res.own[i] = DecideMirrorOwnership(it.owner, it.item, elsewhere);
        if (res.own[i] == MirrorOwnership::Adopt)
            for (const std::string& g : it.guids) res.claimed.emplace(g, it.item);
    }
    return res;
}

std::map<std::string, std::string> NextMirrorOwners(const std::vector<MirrorHeld>& held,
                                                    const std::map<std::string, std::string>& prev_owners,
                                                    const std::vector<std::string>& present,
                                                    const std::vector<std::string>& read_items)
{
    std::vector<std::string> pres = present, rd = read_items;
    std::sort(pres.begin(), pres.end());
    std::sort(rd.begin(), rd.end());
    std::map<std::string, std::string> out;
    for (const auto& o : prev_owners)  // an item present but not read now keeps its markers
        if (std::binary_search(pres.begin(), pres.end(), o.second) && !std::binary_search(rd.begin(), rd.end(), o.second))
            out.emplace(o.first, o.second);
    for (const MirrorHeld& h : held)
        if (h.managed && h.holds)
            for (const std::string& g : h.guids) out[g] = h.item;
    return out;
}

std::vector<std::string> MirrorClaimedNow(const std::vector<MirrorHeld>& held)
{
    std::vector<std::string> out;
    for (const MirrorHeld& h : held)
        if (h.holds)
            for (const std::string& g : h.guids) out.push_back(g);
    return out;
}

std::vector<std::string> MirrorOrphans(const std::vector<std::pair<std::string, std::string>>& prev,
                                       const std::vector<std::string>& claimed,
                                       const std::vector<std::string>& present_items,
                                       const std::vector<std::string>& read_items)
{
    auto sorted = [](std::vector<std::string> v) {
        std::sort(v.begin(), v.end());
        return v;
    };
    const std::vector<std::string> cl = sorted(claimed), pr = sorted(present_items), rd = sorted(read_items);
    auto has = [](const std::vector<std::string>& v, const std::string& s) { return std::binary_search(v.begin(), v.end(), s); };
    std::vector<std::string> out;
    for (const auto& p : prev) {
        if (p.first.empty() || has(cl, p.first)) continue;
        if (has(pr, p.second) && !has(rd, p.second)) continue;  // present but not read now: kept
        out.push_back(p.first);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
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
