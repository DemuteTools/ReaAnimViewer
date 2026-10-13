// SPDX-License-Identifier: MIT
//
// See item_clear_plan.h.

#include "item_clear_plan.h"

#include <algorithm>
#include <set>

#include "rule_record.h"
#include "sound_record.h"

namespace rav {
namespace {

const char kDot[] = " \xC2\xB7 ";  // " · "

std::string Upper(const std::string& s)
{
    std::string u = s;
    for (char& c : u)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return u;
}

std::string Plural(int n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

std::vector<std::string> LibrarySourceGuids(const SoundStore& store)
{
    std::vector<std::string> out;
    for (const SoundLibrary& lib : store.libraries)
        for (const SoundNode& node : lib.nodes)
            if (node.type == SoundNodeType::Sound && !node.sound.source.item_guid.empty())
                out.push_back(node.sound.source.item_guid);
    return out;
}

std::string PoolOnlyRecord(const std::string& raw)
{
    ItemRules r;
    if (raw.empty() || !ParseItemRules(raw, &r) || !r.has_pool) return std::string();
    ItemRules p;
    p.format_version = r.format_version;
    p.header_rest = r.header_rest;
    p.has_pool = true;
    p.pool_id = r.pool_id;
    p.pool_kept = r.pool_kept;
    return SerializeItemRules(p);
}

ItemClearPlan PlanItemClear(const std::vector<ClearTarget>& targets, const std::vector<ClearTagged>& tagged,
                            const std::vector<std::string>& library_source_guids)
{
    ItemClearPlan plan;
    plan.counts.items = static_cast<int>(targets.size());
    plan.counts.copies = std::max(0, plan.counts.items - 1);
    plan.markers.assign(targets.size(), false);

    std::set<std::string> owners;  // the targets' GUIDs
    std::set<std::string> listed;  // every GUID a target's sound record places
    for (size_t i = 0; i < targets.size(); ++i) {
        const ClearTarget& t = targets[i];
        if (!t.guid.empty()) owners.insert(Upper(t.guid));
        ClearRecordWrite w;
        w.target = i;
        if (!t.rules_raw.empty()) {
            ItemRules r;
            if (ParseItemRules(t.rules_raw, &r)) {
                for (const EventEntry& e : r.events)
                    if (e.kind == EventKind::Detected || e.kind == EventKind::User) ++plan.counts.events;
                plan.counts.rules += static_cast<int>(r.blocks.size());
                int marks = static_cast<int>(r.tmarkers.size() + r.ptmarkers.size());
                for (const ProjectMarkerRef& m : r.pmarkers) marks += m.guid.empty() ? 0 : 1;
                for (const ProjectMarkerRef& m : r.ppmarkers) marks += m.guid.empty() ? 0 : 1;
                plan.counts.markers += marks;
                plan.markers[i] = !r.tmarkers.empty() || !r.ptmarkers.empty() || !r.pmarkers.empty() ||
                                  !r.ppmarkers.empty();
            }
            // Made unique: the header and the pool line stay (an unreadable record has none: it goes).
            const std::string keep = PoolOnlyRecord(t.rules_raw);
            if (keep != t.rules_raw) {
                w.rules = true;
                w.rules_text = keep;
            }
        }
        w.committed = !t.committed_raw.empty();
        w.sounds = !t.sounds_raw.empty();
        if (w.sounds) {
            ItemSounds s;
            if (ParseItemSounds(t.sounds_raw, &s))
                for (const EventSounds& ev : s.events)
                    for (const SoundPlacement& p : ev.placed)
                        if (!p.item_guid.empty()) listed.insert(Upper(p.item_guid));
        }
        if (w.rules || w.committed || w.sounds) plan.records.push_back(std::move(w));
    }

    std::set<std::string> sources;
    for (const std::string& g : library_source_guids)
        if (!g.empty()) sources.insert(Upper(g));

    std::set<std::string> done;  // each item once
    for (const ClearTagged& t : tagged) {
        const std::string key = Upper(t.guid);
        if (key.empty() || done.count(key)) continue;
        PlacedTag  tag;
        const bool reads = !t.tag_raw.empty() && ParsePlacedTag(t.tag_raw, &tag);
        const bool owned = reads && owners.count(Upper(tag.owner)) > 0;
        if (!owned && !listed.count(key)) continue;  // another item's sound (or the user's)
        done.insert(key);
        if (sources.count(key)) {
            plan.untag_guids.push_back(t.guid);  // a Project Library Sound's source: it stays
            continue;
        }
        plan.delete_guids.push_back(t.guid);
        ++plan.counts.sounds;
        if (reads && tag.own == Ownership::Attached) ++plan.counts.attached;
    }

    plan.anything = !plan.records.empty() || !plan.delete_guids.empty() || !plan.untag_guids.empty();
    return plan;
}

std::string ClearConfirmTitle(int copies)
{
    if (copies <= 0) return "Clear this item?";
    return "Clear this item and its " + std::to_string(copies) + " linked " + (copies == 1 ? "copy?" : "copies?");
}

std::string ClearConfirmText(const ItemClearCounts& c)
{
    std::vector<std::string> parts;
    int                      last = 0;  // the count of the last part (a lone "1 rule" is removed)
    auto add = [&](int n, const char* one, const char* many) {
        if (n <= 0) return;
        parts.push_back(Plural(n, one, many));
        last = n;
    };
    add(c.events, "event", "events");
    add(c.rules, "rule", "rules");
    add(c.markers, "marker", "markers");
    add(c.sounds, "sound", "sounds");
    if (c.sounds > 0 && c.attached > 0) parts.back() += " (" + std::to_string(c.attached) + " attached by you)";
    if (parts.empty()) return "Its settings are removed. One undo step.";
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? kDot : "") + parts[i];
    return out + (parts.size() == 1 && last == 1 ? " is" : " are") + " removed. One undo step.";
}

std::string ClearDoneText(const ItemClearCounts& c)
{
    std::string out = "Cleared: 1 item";
    if (c.copies > 0) out += " + " + Plural(c.copies, "copy", "copies");
    if (c.sounds > 0) out += kDot + Plural(c.sounds, "sound", "sounds") + " removed";
    return out;
}

}  // namespace rav
