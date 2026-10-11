// SPDX-License-Identifier: MIT
//
// See sound_attach_plan.h (13-10).

#include "sound_attach_plan.h"

#include <algorithm>
#include <cmath>

#include "sound_draw.h"
#include "sound_record.h"

namespace rav {

std::string AttachSkipReason(bool rav_item, bool folder, bool has_tag, Ownership own)
{
    if (rav_item) return "animation item";
    if (folder) return "folder item";
    if (has_tag && own != Ownership::Attached) return "placed by RAV";
    return {};
}

std::vector<std::vector<int>> OverlapGroups(const std::vector<AttachItem>& items)
{
    std::vector<int> order(items.size());
    for (size_t i = 0; i < items.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return items[a].pos < items[b].pos; });
    std::vector<std::vector<int>> out;
    double                        end = 0.0;
    for (int i : order) {
        const AttachItem& it = items[static_cast<size_t>(i)];
        if (out.empty() || it.pos > end) {
            out.push_back({i});
            end = it.pos + it.len;
        } else {
            out.back().push_back(i);
            end = std::max(end, it.pos + it.len);
        }
    }
    return out;
}

int TopmostOf(const std::vector<AttachItem>& items, const std::vector<int>& indices)
{
    int best = -1;
    for (int i : indices) {
        if (i < 0 || static_cast<size_t>(i) >= items.size()) continue;
        if (best < 0) {
            best = i;
            continue;
        }
        const AttachItem& a = items[static_cast<size_t>(i)];
        const AttachItem& b = items[static_cast<size_t>(best)];
        if (a.track < b.track || (a.track == b.track && a.pos < b.pos)) best = i;
    }
    return best;
}

int NearestEvent(const std::vector<AttachEvent>& events, double anchor, int block)
{
    int    best = -1;
    double best_d = 0.0;
    for (size_t i = 0; i < events.size(); ++i) {
        const AttachEvent& e = events[i];
        if (block >= 0 && e.block != block) continue;
        const double d = std::fabs(e.project_t - anchor);
        if (best < 0 || d < best_d - 1e-12
            || (std::fabs(d - best_d) <= 1e-12 && e.project_t < events[static_cast<size_t>(best)].project_t)) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    return best;
}

bool PileOnEvent(double anchor, double event_project_t)
{
    return std::fabs(anchor - event_project_t) <= kEventMatchSeconds + 1e-9;
}

double SpreadMemberOffset(double anchor, double pos, double snap, double nudge)
{
    return nudge + pos + snap - anchor;
}

double PromoteCopyPosition(double project_end, double first_pos, double member_pos)
{
    return project_end + 1.0 + (member_pos - first_pos);
}

double BlendChildOffsetMs(double member_sync, double top_sync)
{
    return (member_sync - top_sync) * 1000.0;
}

int SpreadTakePick(int takes, const std::vector<int>& history, uint64_t seed, uint64_t salt)
{
    if (takes < 2) return -1;
    std::vector<int> active;
    for (int i = 0; i < takes; ++i) active.push_back(i);
    return DrawPick(active, 2, history, seed, salt);
}

std::vector<AttachPilePlan> PlanAttach(const std::vector<AttachItem>& items, const std::vector<AttachEvent>& events,
                                       int block)
{
    std::vector<std::vector<int>> piles;
    if (block < 0) {
        std::vector<int> all;
        for (size_t i = 0; i < items.size(); ++i) all.push_back(static_cast<int>(i));
        if (!all.empty()) piles.push_back(all);
    } else {
        piles = OverlapGroups(items);
    }
    std::vector<AttachPilePlan> out;
    for (const std::vector<int>& members : piles) {
        AttachPilePlan p;
        p.members = members;
        const int top = TopmostOf(items, members);
        if (top < 0) continue;
        p.anchor = PileAnchor(items[static_cast<size_t>(top)]);
        p.event = NearestEvent(events, p.anchor, block);
        if (p.event >= 0) p.offset_s = p.anchor - events[static_cast<size_t>(p.event)].project_t;
        out.push_back(p);
    }
    return out;
}

SoundPlacement AttachPlacementOf(const AttachItem& item, double offset_s)
{
    SoundPlacement p;
    p.item_guid = item.guid;
    p.own = Ownership::Attached;
    p.offset_s = offset_s;
    p.track_guid = item.track_guid;
    return p;
}

PlacedTag AttachTagOf(const std::string& owner, const AttachEvent& ev)
{
    PlacedTag tag;
    tag.owner = owner;
    tag.rid = ev.rid;
    tag.t = ev.clip_t;
    tag.own = Ownership::Attached;
    return tag;
}

std::vector<AttachEvent> SpreadTargets(const std::vector<AttachEvent>& events, int block, double anchor,
                                       const double* attached_clip_t)
{
    std::vector<AttachEvent> out;
    for (const AttachEvent& e : events) {
        if (e.block != block || PileOnEvent(anchor, e.project_t)) continue;
        if (attached_clip_t && std::fabs(e.clip_t - *attached_clip_t) <= kEventMatchSeconds) continue;
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const AttachEvent& a, const AttachEvent& b) { return a.project_t < b.project_t; });
    return out;
}

std::vector<TakenCopy> TakeSpreadCopies(ItemSounds& rec, const std::string& rid, const std::vector<AttachEvent>& targets,
                                        std::vector<double>* nudges)
{
    std::vector<TakenCopy> out;
    nudges->assign(targets.size(), 0.0);
    for (size_t k = 0; k < targets.size(); ++k) {
        bool have_nudge = false;
        if (EventSounds* ev = FindEventSounds(rec, rid, targets[k].clip_t)) {
            for (auto it = ev->placed.begin(); it != ev->placed.end();) {
                if (it->own != Ownership::Spread) {
                    ++it;
                    continue;
                }
                if (!have_nudge) {
                    (*nudges)[k] = it->offset_s;
                    have_nudge = true;
                }
                out.push_back({*it, ev->t});
                it = ev->placed.erase(it);
            }
        }
        // The other project times of the same event keep its nudge.
        if (!have_nudge)
            for (size_t j = 0; j < k; ++j)
                if (std::fabs(targets[j].clip_t - targets[k].clip_t) <= kEventMatchSeconds) (*nudges)[k] = (*nudges)[j];
    }
    return out;
}

int SpreadPickTakes(const std::vector<AttachItem>& items)
{
    for (const AttachItem& f : items)
        if (f.takes >= 2) return f.takes;
    return 0;
}

std::vector<int> SpreadPicks(const std::vector<AttachEvent>& targets, int takes, uint64_t seed, const std::string& rid)
{
    std::vector<int> picks(targets.size(), -1);
    if (takes < 2) return picks;
    std::vector<int> history;
    for (size_t k = 0; k < targets.size(); ++k) {
        bool shared = false;
        for (size_t j = 0; j < k && !shared; ++j)
            if (std::fabs(targets[j].clip_t - targets[k].clip_t) <= kEventMatchSeconds) {
                picks[k] = picks[j];
                shared = true;
            }
        if (shared) continue;
        picks[k] = SpreadTakePick(takes, history, seed, EventSalt(rid, targets[k].clip_t));
        history.push_back(picks[k]);
    }
    return picks;
}

bool DetachFromRecord(ItemSounds& rec, const std::string& guid)
{
    bool removed = false;
    for (EventSounds& e : rec.events) {
        const size_t before = e.placed.size();
        e.placed.erase(std::remove_if(e.placed.begin(), e.placed.end(),
                                      [&](const SoundPlacement& p) { return p.item_guid == guid; }),
                       e.placed.end());
        removed = removed || e.placed.size() != before;
    }
    if (removed) {
        rec.events.erase(std::remove_if(rec.events.begin(), rec.events.end(),
                                        [](const EventSounds& e) {
                                            return e.placed.empty() && e.draws.empty() && e.overrides.empty()
                                                && e.kept.empty() && e.kept_lines.empty();
                                        }),
                         rec.events.end());
    }
    return removed;
}

void AddToRecord(ItemSounds& rec, const std::string& rid, double t, const SoundPlacement& p)
{
    EventSounds* e = FindEventSounds(rec, rid, t);
    if (!e) {
        EventSounds n;
        n.rid = rid;
        n.t = t;
        rec.events.push_back(n);
        e = &rec.events.back();
    }
    e->placed.push_back(p);
}

namespace {

// The index of a node in the library's list, -1 when absent.
int NodeIndex(const SoundLibrary& lib, const std::string& uid)
{
    for (size_t i = 0; i < lib.nodes.size(); ++i)
        if (lib.nodes[i].uid == uid) return static_cast<int>(i);
    return -1;
}

SoundNode NewRandom(const std::string& uid, const std::string& parent, const std::string& name)
{
    SoundNode r;
    r.uid = uid;
    r.parent = parent;
    r.type = SoundNodeType::Random;
    r.name = name;
    return r;
}

}  // namespace

bool AddPromotedToLibrary(SoundLibrary& lib, const std::vector<SoundNode>& nodes, const std::string& value_uid,
                          const std::string& new_uid, std::string* error, const std::string& random_name)
{
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (nodes.empty() || nodes[0].uid.empty()) return fail("nothing to add");
    std::vector<SoundNode> add = nodes;
    std::string            parent;
    const int              root_i = lib.root.empty() ? -1 : NodeIndex(lib, lib.root);
    if (root_i < 0) {
        // No root: the new node is the root.
        add[0].parent.clear();
        lib.root = add[0].uid;
        lib.nodes.insert(lib.nodes.end(), add.begin(), add.end());
        return true;
    }
    SoundNode& root = lib.nodes[static_cast<size_t>(root_i)];
    switch (root.type) {
    case SoundNodeType::Random:
    case SoundNodeType::Blend:
        parent = root.uid;
        break;
    case SoundNodeType::Sound: {
        if (new_uid.empty()) return fail("no id for the new Random");
        root.parent = new_uid;
        lib.nodes.insert(lib.nodes.begin(), NewRandom(new_uid, "", random_name.empty() ? lib.name : random_name));
        lib.root = new_uid;
        parent = new_uid;
        break;
    }
    case SoundNodeType::Switch: {
        if (value_uid.empty()) return fail("pick a value of the Switch");
        SwitchCase* c = nullptr;
        for (SwitchCase& sc : root.cases)
            if (sc.value_uid == value_uid) c = &sc;
        if (!c || c->child_uid.empty() || NodeIndex(lib, c->child_uid) < 0) {
            if (c) c->child_uid = add[0].uid;
            else {
                SwitchCase n;
                n.value_uid = value_uid;
                n.child_uid = add[0].uid;
                root.cases.push_back(n);
            }
            parent = root.uid;
            break;
        }
        const int child_i = NodeIndex(lib, c->child_uid);
        if (lib.nodes[static_cast<size_t>(child_i)].type == SoundNodeType::Random) {
            parent = c->child_uid;
            break;
        }
        if (new_uid.empty()) return fail("no id for the new Random");
        const std::string root_uid = root.uid;
        c->child_uid = new_uid;
        lib.nodes[static_cast<size_t>(child_i)].parent = new_uid;
        // The new Random's line goes just before its first child's (children after their parent).
        lib.nodes.insert(lib.nodes.begin() + child_i, NewRandom(new_uid, root_uid, random_name.empty() ? lib.name : random_name));
        parent = new_uid;
        break;
    }
    }
    add[0].parent = parent;
    lib.nodes.insert(lib.nodes.end(), add.begin(), add.end());
    return true;
}

}  // namespace rav
