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

void KeepBlockEvents(std::vector<Event>& detections, const std::vector<EventEntry>& entries,
                     const std::vector<Block>& blocks, const std::vector<char>& keep)
{
    auto kept = [&](int b) {
        return b >= 0 && b < static_cast<int>(blocks.size()) && static_cast<size_t>(b) < keep.size() &&
               keep[static_cast<size_t>(b)] != 0;
    };
    if (std::none_of(keep.begin(), keep.end(), [](char k) { return k != 0; })) return;
    detections.erase(std::remove_if(detections.begin(), detections.end(), [&](const Event& e) { return kept(e.block); }),
                     detections.end());
    for (const EventEntry& x : entries) {
        if (x.kind != EventKind::Detected || !kept(x.block)) continue;
        Event e;
        e.time_s = x.t;
        e.block = x.block;
        e.marker = blocks[static_cast<size_t>(x.block)].marker;
        e.strength = x.strength;
        e.speed = x.speed;
        detections.push_back(e);
    }
    std::stable_sort(detections.begin(), detections.end(), [](const Event& a, const Event& b) {
        return a.time_s < b.time_s || (a.time_s == b.time_s && a.block < b.block);
    });
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
    // 10-6: the pool is the item's as it is now (a snapshot taken before Make unique never links
    // the item back).
    out.has_pool = current.has_pool;
    out.pool_id = current.pool_id;
    out.pool_kept = current.pool_kept;
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
            if (st.restore) {
                if (in) {
                    step.action = MirrorAction::Show;
                    step.new_t = pt;
                } else {
                    step.action = MirrorAction::Hide;
                }
            } else if (r.has_name) {
                // 10-4c: deleted by the user. Its event now outside the item: hidden (nothing to
                // delete), not read as a delete.
                step.action = in ? MirrorAction::UserDelete : MirrorAction::Hide;
            }
            continue;
        }
        // 10-4c: renamed by the user: the marker is theirs.
        if (r.has_name && st.has_name && st.name != r.name) {
            step.action = MirrorAction::UserDisown;
            continue;
        }
        if (in && std::fabs(st.now_t - pt) <= kMirrorPosTolS) {
            // In place (an undo, a ripple that moved it with its item): only `t` follows.
            if (std::fabs(st.now_t - r.t) > kMirrorPosTolS) {
                step.action = MirrorAction::Retime;
                step.new_t = st.now_t;
            }
            continue;
        }
        if (std::fabs(st.now_t - r.t) <= kMirrorPosTolS) {
            // Still where RAV put it: its item changed (move, trim, rate, offset).
            if (!in) {
                step.action = MirrorAction::Hide;
                step.delete_old = true;
            } else {
                step.action = MirrorAction::Move;
                step.new_t = pt;
            }
            continue;
        }
        // At neither place. The item changed too (a tempo / timebase change, a ripple that moved
        // the marker by another amount): the item wins, the marker is placed from it.
        if (map_changed) {
            if (!in) {
                step.action = MirrorAction::Hide;
                step.delete_old = true;
            } else {
                step.action = MirrorAction::Move;
                step.new_t = pt;
            }
            continue;
        }
        // The item unchanged: the user dragged it (10-4c). Outside the item: theirs.
        if (!r.has_name) continue;
        double nc = 0.0;
        if (FirstPassClipTime(map, st.now_t, &nc)) {
            step.action = MirrorAction::UserDrag;
            step.new_t = st.now_t;
            step.new_c = nc;
        } else {
            step.action = MirrorAction::UserDisown;
        }
    }
    return out;
}

namespace {

constexpr double kEditMatchTolS = 1e-5;  // an entry this close to a marker's clip time is its event

// The marker name a rule writes, committed or preview.
std::string EditNameOf(const Block& b, bool preview)
{
    const std::string n = OneLine(b.marker);
    return preview ? PreviewMarkerName(n) : n;
}

int FindEntryNear(const std::vector<EventEntry>& entries, EventKind kind, int block, double t, double tol)
{
    int    best = -1;
    double bd = 1e300;
    for (size_t i = 0; i < entries.size(); ++i) {
        const EventEntry& x = entries[i];
        if (x.kind != kind || x.block != block) continue;
        const double d = std::fabs(x.t - t);
        if (d <= tol + 1e-12 && d < bd) {
            bd = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

}  // namespace

int MarkerEditBlock(const ItemRules& rules, const MarkerEdit& e)
{
    std::vector<int> cand;
    for (size_t i = 0; i < rules.blocks.size(); ++i)
        if (EditNameOf(rules.blocks[i], e.preview) == e.name) cand.push_back(static_cast<int>(i));
    if (e.has_color && cand.size() > 1) {
        std::vector<int> col;
        for (int b : cand) {
            const uint32_t c = rules.blocks[static_cast<size_t>(b)].color;
            if ((e.preview ? PreviewColor(c) : c) == e.color) col.push_back(b);
        }
        if (!col.empty()) cand = col;
    }
    if (cand.empty()) return -1;
    if (cand.size() > 1) {
        for (int b : cand)
            if (FindEntryNear(rules.events, EventKind::User, b, e.c, kEditMatchTolS) >= 0) return b;
        for (int b : cand)
            if (FindEntryNear(rules.events, EventKind::Detected, b, e.c, kEditMatchTolS) >= 0) return b;
    }
    return cand.front();
}

ItemRules ApplyMarkerEdits(const ItemRules& rules, const std::vector<MarkerEdit>& edits,
                           const std::vector<Event>* detections, std::vector<int>* blocks)
{
    ItemRules out = rules;
    if (blocks) blocks->assign(edits.size(), -1);
    std::vector<std::pair<int, double>> done;  // (rule, clip time) of the events edited
    for (size_t k = 0; k < edits.size(); ++k) {
        const MarkerEdit& e = edits[k];
        if (!std::isfinite(e.c) || (e.kind == MarkerEditKind::Drag && !std::isfinite(e.new_c))) continue;
        const int b = MarkerEditBlock(out, e);
        if (b < 0) continue;
        bool again = false;
        for (const auto& d : done)
            if (d.first == b && std::fabs(d.second - e.c) <= kEditMatchTolS) again = true;
        if (again) continue;
        done.push_back({b, e.c});
        if (blocks) (*blocks)[k] = b;
        std::vector<EventEntry>& ev = out.events;
        const int user = FindEntryNear(ev, EventKind::User, b, e.c, kEditMatchTolS);
        if (user >= 0) {
            // A user event: it moves, or goes.
            if (e.kind == MarkerEditKind::Drag) ev[static_cast<size_t>(user)].t = e.new_c;
            else ev.erase(ev.begin() + user);
            continue;
        }
        // A detection: suppressed where it was (once), and on a drag the user's own event at new_c
        // with the detection's values.
        if (FindEntryNear(ev, EventKind::Suppress, b, e.c, kEditMatchTolS) < 0) ev.push_back(MakeSuppression(b, e.c));
        if (e.kind != MarkerEditKind::Drag) continue;
        double     s = 0.0, v = 0.0;
        const int  snap = FindEntryNear(ev, EventKind::Detected, b, e.c, kEditMatchTolS);
        if (snap >= 0) {
            s = ev[static_cast<size_t>(snap)].strength;
            v = ev[static_cast<size_t>(snap)].speed;
        } else if (detections) {
            double bd = 1e300;
            for (const Event& d : *detections) {
                const double dist = std::fabs(d.time_s - e.c);
                if (d.block == b && dist <= 0.001 + 1e-12 && dist < bd) {
                    bd = dist;
                    s = d.strength;
                    v = d.speed;
                }
            }
        }
        ev.push_back(MakeUserEvent(b, e.new_c, s, v));
    }
    return out;
}

int FindTakeTwin(const std::vector<TakeMarkerRef>& tmarkers, double c, const std::string& name, double tol,
                 const std::vector<char>& skip)
{
    for (size_t i = 0; i < tmarkers.size(); ++i)
        if (!(i < skip.size() && skip[i]) && tmarkers[i].name == name && std::fabs(tmarkers[i].t - c) <= tol)
            return static_cast<int>(i);
    return -1;
}

int FindProjectTwin(const std::vector<ProjectMarkerRef>& pmarkers, double c, const std::string& name, double tol,
                    const std::vector<char>& skip)
{
    for (size_t i = 0; i < pmarkers.size(); ++i) {
        const ProjectMarkerRef& p = pmarkers[i];
        if (!(i < skip.size() && skip[i]) && p.has_c && p.name == name && std::fabs(p.c - c) <= tol)
            return static_cast<int>(i);
    }
    return -1;
}

std::vector<TakeRefMatch> MatchTakeRefs(const std::vector<TakeMarkerRef>& refs,
                                        const std::vector<ExistingMarker>& markers, double tol)
{
    std::vector<TakeRefMatch> out(refs.size());
    std::vector<char>         used(markers.size(), 0);
    std::vector<char>         placed(refs.size(), 0);
    auto at = [&](double a, double b) { return std::fabs(a - b) <= tol + 1e-12; };
    // In place: its name at its time.
    for (size_t r = 0; r < refs.size(); ++r)
        for (size_t m = 0; m < markers.size(); ++m)
            if (!used[m] && markers[m].name == refs[r].name && at(markers[m].t, refs[r].t)) {
                used[m] = placed[r] = 1;
                out[r] = {TakeRefFate::InPlace, static_cast<int>(m)};
                break;
            }
    // Renamed: a marker no ref matches at its time, with another name.
    for (size_t r = 0; r < refs.size(); ++r) {
        if (placed[r]) continue;
        for (size_t m = 0; m < markers.size(); ++m)
            if (!used[m] && markers[m].name != refs[r].name && at(markers[m].t, refs[r].t)) {
                used[m] = placed[r] = 1;
                out[r] = {TakeRefFate::Rename, static_cast<int>(m)};
                break;
            }
    }
    // Dragged: the nearest marker no ref matches with its name.
    for (size_t r = 0; r < refs.size(); ++r) {
        if (placed[r]) continue;
        int    best = -1;
        double bd = 1e300;
        for (size_t m = 0; m < markers.size(); ++m) {
            if (used[m] || markers[m].name != refs[r].name) continue;
            const double d = std::fabs(markers[m].t - refs[r].t);
            if (d < bd) {
                bd = d;
                best = static_cast<int>(m);
            }
        }
        if (best >= 0) {
            used[static_cast<size_t>(best)] = placed[r] = 1;
            out[r] = {TakeRefFate::Drag, best};
        } else {
            out[r] = {TakeRefFate::Delete, -1};
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

// ---- 10-6: pooled copies ------------------------------------------------------------------------

bool CancelCopyCancellable(const CancelCopy& c)
{
    return c.readable && (!c.rules.blocks.empty() || c.has_snap || HasPreviews(c.rules));
}

PoolCancelPlan PlanPoolCancel(const std::vector<CancelCopy>& copies)
{
    PoolCancelPlan plan;
    int            from = -1;
    for (size_t i = 0; i < copies.size() && from < 0; ++i)
        if (copies[i].shown && CancelCopyCancellable(copies[i])) from = static_cast<int>(i);
    for (size_t i = 0; i < copies.size() && from < 0; ++i)
        if (copies[i].selected && CancelCopyCancellable(copies[i])) from = static_cast<int>(i);
    for (size_t i = 0; i < copies.size() && from < 0; ++i)
        if (CancelCopyCancellable(copies[i])) from = static_cast<int>(i);
    plan.from = from;
    if (from < 0) {
        for (const CancelCopy& c : copies)
            if (c.selected) ++plan.without_rules;
        return plan;
    }
    // The copy it runs from: its last Commit; nothing to cancel there = its content as it is.
    const CancelCopy& f = copies[static_cast<size_t>(from)];
    ItemRules         target;
    if (CancelTarget(f.rules, f.raw, f.has_snap ? &f.snap_text : nullptr, &target))
        plan.writes.push_back({static_cast<size_t>(from), target});
    else
        target = f.rules;
    // Every other copy: the same content, no preview, its own bookkeeping.
    for (size_t i = 0; i < copies.size(); ++i) {
        const CancelCopy& c = copies[i];
        if (static_cast<int>(i) == from) continue;
        if (!c.readable) {
            if (c.selected) ++plan.without_rules;  // no record (nothing to cancel), or unreadable
            continue;
        }
        ItemRules next = c.rules;
        CopyPoolContent(target, next);
        ClearPreviewed(next);
        if (!HasPreviews(c.rules) && SerializeItemRules(next) == c.raw) continue;  // nothing to cancel
        plan.writes.push_back({i, std::move(next)});
    }
    return plan;
}

ItemRules MergePoolEdits(const ItemRules& first, const std::vector<CopyEdits>& others)
{
    ItemRules content = first;
    for (const CopyEdits& o : others)
        content.events = ApplyMarkerEdits(content, o.all, o.det_ok ? &o.detections : nullptr).events;
    return content;
}

std::vector<MarkerEdit> SnapshotEditsFor(const std::vector<CopyEdits>& edited, int own)
{
    std::vector<MarkerEdit> out;
    for (size_t i = 0; i < edited.size(); ++i)
        if (static_cast<int>(i) != own) out.insert(out.end(), edited[i].committed.begin(), edited[i].committed.end());
    return out;
}

ItemRules FollowerRecord(const ItemRules& cur, const ItemRules& content, bool replaced,
                         const std::vector<TakeMarkerRef>& own_take, const std::vector<ProjectMarkerRef>& own_project,
                         bool keep_take_refs, bool keep_project_refs, const std::vector<PlannedMarker>& plan,
                         MarkerMode mode, const std::string& self_guid)
{
    ItemRules next = cur;
    CopyPoolContent(content, next);
    if (replaced) {
        ComposeCommittedRecord(next, own_take, own_project, keep_take_refs, keep_project_refs, plan, mode);
        next.events = content.events;  // the pool's, as they are
    }
    if (!self_guid.empty()) SetRecordOwner(next, self_guid);
    return next;
}

ItemRules FollowerSnapshot(const ItemRules& snap, const std::vector<MarkerEdit>& edits,
                           const std::vector<Event>* detections, const ItemRules& rec, const std::string& new_sig)
{
    ItemRules out = snap;
    out.events = ApplyMarkerEdits(snap, edits, detections).events;
    out.pmarkers = rec.pmarkers;
    out.tmarkers = rec.tmarkers;
    if (!new_sig.empty() && out.has_applied) out.applied.sig = new_sig;
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
