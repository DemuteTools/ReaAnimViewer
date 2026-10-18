// SPDX-License-Identifier: MIT
//
// See sound_spread_plan.h (13-68 v2 / v3).

#include "sound_spread_plan.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

#include "sound_place_plan.h"  // SourceSecondsOf
#include "event_list.h"    // kPreviewSuffix
#include "sound_record.h"  // FindNode

namespace rav {

std::vector<std::vector<int>> SpreadPiles(const std::vector<AttachItem>& items)
{
    std::vector<std::vector<int>> piles = OverlapGroups(items, true);
    for (std::vector<int>& p : piles)
        std::stable_sort(p.begin(), p.end(), [&](int a, int b) {
            const AttachItem& x = items[static_cast<size_t>(a)];
            const AttachItem& y = items[static_cast<size_t>(b)];
            return x.track != y.track ? x.track < y.track : x.pos < y.pos;
        });
    return piles;
}

void SyncFromSnap(SoundData& d, double snap_s)
{
    d.sync_mode = SyncMode::SliceStart;
    d.sync_offset_ms = std::isfinite(snap_s) ? std::max(0.0, snap_s) * 1000.0 : 0.0;
}

namespace {

// A GUID compared without case.
std::string GuidUpper(std::string g)
{
    for (char& c : g) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return g;
}

// A group's default value (its own when it holds it, else the first; GroupDefaultValue, kept here: no sound_switch).
std::string GroupDefaultValueOf(const SwitchGroup& g)
{
    for (const SwitchValue& v : g.values)
        if (!g.default_value.empty() && v.uid == g.default_value) return v.uid;
    return g.values.empty() ? std::string() : g.values.front().uid;
}

// 13-68 v3 review: a value's name ("" when the group lacks it).
std::string FindValueName(const SwitchGroup& g, const std::string& uid)
{
    for (const SwitchValue& v : g.values)
        if (v.uid == uid) return v.name;
    return std::string();
}

// 13-68 v3 review: a Switch's default value (its own when its group holds it, else the group's: DefaultValue, kept
// here: no sound_switch).
std::string SwitchDefaultOf(const SwitchGroup& g, const SoundNode& sw)
{
    for (const SwitchValue& v : g.values)
        if (!sw.default_value.empty() && v.uid == sw.default_value) return v.uid;
    return GroupDefaultValueOf(g);
}

SoundNode SoundOf(const SpreadSource& s, const std::string& uid, const std::string& parent)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = SoundNodeType::Sound;
    n.name = s.name.empty() ? std::string("Sound") : s.name;
    n.sound = s.sound;
    // 13-68 v3: its copies play at its take's rate (keep_rate, plain and folder piles); the sync in source seconds, which
    // the copy reads back over 1 / rate (ApplyPiece), so its snap lands on the marker.
    n.sound.keep_rate = true;
    SyncFromSnap(n.sound, SourceSecondsOf(s.facts.snap, s.playrate));
    return n;
}

}  // namespace

SoundData PileSoundData(const std::string& folder_guid, int takes, double len, const std::vector<Slice>& slices)
{
    SoundData d;
    d.source.kind = SoundSourceKind::Item;
    d.source.item_guid = folder_guid;
    d.linked_copies = true;
    if (slices.size() >= 2) {  // take markers (NVK): one variation per slice of the top member
        d.mode = DetectMode::TakeMarkers;
        for (const Slice& s : slices) {
            SoundFrame f;
            f.start = s.start;
            f.length = s.length;
            d.frames.push_back(f);
        }
        return d;
    }
    d.mode = takes >= 2 ? DetectMode::Takes : DetectMode::Whole;
    const double l = std::isfinite(len) ? std::max(len, 0.0) : 0.0;
    for (int k = 0; k < std::max(takes, 1); ++k) {
        SoundFrame f;
        f.length = l;
        d.frames.push_back(f);
    }
    return d;
}

std::vector<SoundNode> SpreadLayerNodes(const std::vector<SpreadSource>& items, const std::string& name,
                                        const std::function<std::string()>& new_uid)
{
    std::vector<SoundNode> out;
    if (items.empty()) return out;
    std::vector<AttachItem> facts;
    for (const SpreadSource& s : items) facts.push_back(s.facts);
    SoundNode root;
    root.uid = new_uid();
    root.type = SoundNodeType::Random;
    root.name = name;
    out.push_back(root);
    for (const std::vector<int>& pile : SpreadPiles(facts)) {
        if (pile.empty()) continue;
        const SpreadSource& top = items[static_cast<size_t>(pile.front())];
        const auto          folder = std::find_if(pile.begin(), pile.end(), [&](int m) { return items[static_cast<size_t>(m)].folder; });
        if (folder != pile.end()) {
            out.push_back(SoundOf(items[static_cast<size_t>(*folder)], new_uid(), root.uid));
            continue;
        }
        if (pile.size() == 1) {
            out.push_back(SoundOf(top, new_uid(), root.uid));
            continue;
        }
        SoundNode blend;
        blend.uid = new_uid();
        blend.parent = root.uid;
        blend.type = SoundNodeType::Blend;
        blend.name = top.name.empty() ? std::string("Sound") : top.name;
        out.push_back(blend);
        const double top_sync = PileAnchor(top.facts);
        for (int m : pile) {
            const SpreadSource& s = items[static_cast<size_t>(m)];
            SoundNode           child = SoundOf(s, new_uid(), blend.uid);
            child.offset_ms = BlendChildOffsetMs(PileAnchor(s.facts), top_sync);
            child.layer_keep_offset = true;
            out.push_back(child);
        }
    }
    return out;
}

SpreadKind SpreadKindOf(const std::string& sound_key, const std::string& side_free, const std::string& marker)
{
    SpreadKind k;
    const std::string m = MarkerTypeName(marker);
    if (!sound_key.empty()) {
        k.key = "auto:" + sound_key.substr(0, sound_key.rfind(':'));
        k.name = MarkerTypeName(side_free).empty() ? m : MarkerTypeName(side_free);
    } else {
        k.key = m;
        k.name = m;
    }
    if (k.name.empty()) k.key.clear();
    return k;
}

int RuleOfMarker(const std::vector<std::string>& rule_markers, const std::string& name)
{
    const std::string n = MarkerTypeName(name);
    for (size_t i = 0; i < rule_markers.size(); ++i) {
        const std::string t = MarkerTypeName(rule_markers[i]);
        if (!t.empty() && (t == n || t + kPreviewSuffix == n)) return static_cast<int>(i);
    }
    return -1;
}

namespace {

bool ServesType(const SoundLibrary& l, const std::string& type)
{
    return l.attached && !l.user && SameMarkerType(l.attached_type.empty() ? l.name : l.attached_type, type);
}

}  // namespace

SoundLibrary* FindAttachedLibrary(SoundStore& store, const std::string& type)
{
    for (SoundLibrary& l : store.libraries)
        if (ServesType(l, type)) return &l;
    return nullptr;
}

const SoundLibrary* FindAttachedLibrary(const SoundStore& store, const std::string& type)
{
    for (const SoundLibrary& l : store.libraries)
        if (ServesType(l, type)) return &l;
    return nullptr;
}

void SetLibraryContent(SoundLibrary& lib, std::vector<SoundNode> nodes)
{
    lib.nodes = std::move(nodes);
    if (!lib.nodes.empty()) lib.nodes[0].parent.clear();
    lib.root = lib.nodes.empty() ? std::string() : lib.nodes[0].uid;
}

namespace {

// The library's root Blend: a root that is not a Blend is wrapped in a new Blend `blend_uid` named after the library
// (the old root its first child, offset 0); no root: that Blend alone becomes the root.
std::string RootBlend(SoundLibrary& lib, const std::string& blend_uid)
{
    const SoundNode* root = lib.root.empty() ? nullptr : FindNode(lib, lib.root);
    if (root && root->type == SoundNodeType::Blend) return lib.root;
    SoundNode b;
    b.uid = blend_uid;
    b.type = SoundNodeType::Blend;
    b.name = lib.name;
    if (SoundNode* old = root ? FindNode(lib, lib.root) : nullptr) {
        old->parent = b.uid;
        old->offset_ms = 0.0;
        old->layer_keep_offset = true;
    }
    lib.nodes.insert(lib.nodes.begin(), b);
    lib.root = b.uid;
    return b.uid;
}

}  // namespace

void AddStackLayer(SoundLibrary& lib, std::vector<SoundNode> layer, double offset_ms, const std::string& blend_uid)
{
    if (layer.empty()) return;
    if (lib.root.empty() || !FindNode(lib, lib.root)) {
        layer[0].offset_ms = 0.0;
        SetLibraryContent(lib, std::move(layer));
        return;
    }
    const std::string blend = RootBlend(lib, blend_uid);
    layer[0].parent = blend;
    layer[0].offset_ms = offset_ms;
    layer[0].layer_keep_offset = true;
    for (SoundNode& n : layer) lib.nodes.push_back(std::move(n));
}

ValueSet AddAnimationLayer(SoundStore& store, const std::string& lib_uid, std::vector<SoundNode> layer, double offset_ms,
                           const std::string& anim_name, const std::vector<ValueSet>& item_values,
                           const std::string& kind_name, const std::function<std::string()>& new_uid,
                           const std::set<std::string>& held_elsewhere)
{
    ValueSet out;
    SoundLibrary* lib = nullptr;
    for (SoundLibrary& l : store.libraries)
        if (l.uid == lib_uid) lib = &l;
    if (!lib || layer.empty()) return out;
    const std::string blend = RootBlend(*lib, new_uid());
    // The library's RAV-owned Switch in the root Blend (one per library), else a new one and its group.
    SoundNode*   sw = nullptr;
    SwitchGroup* g = nullptr;
    for (SoundNode& n : lib->nodes)
        if (n.parent == blend && n.type == SoundNodeType::Switch)
            if (SwitchGroup* ng = FindGroup(store, n.group_uid); ng && ng->rav_owned) {
                sw = &n;
                g = ng;
                break;
            }
    if (!sw) {
        const std::string gname = (kind_name.empty() ? lib->name : kind_name) + " layers";
        for (SwitchGroup& og : store.groups)  // 13-68 v3 review: a RAV group of that name (left by an earlier Switch) reused
            if (!g && og.rav_owned && og.name == gname) g = &og;
        if (!g) {  // 13-68 v3: else a new one
            SwitchGroup ng;
            ng.uid = new_uid();
            ng.name = gname;  // 13-68 v3
            ng.rav_owned = true;
            SwitchValue none;  // the default: adds nothing (no case)
            none.uid = new_uid();
            none.name = "Other animations";
            ng.values.push_back(none);
            ng.default_value = none.uid;
            store.groups.push_back(ng);
            g = &store.groups.back();
        }
        SoundNode n;
        n.uid = new_uid();
        n.parent = blend;
        n.type = SoundNodeType::Switch;
        n.name = g->name;      // 13-68 v3: g
        n.group_uid = g->uid;  // 13-68 v3: g
        n.layer_keep_offset = true;
        lib->nodes.push_back(n);
        sw = &lib->nodes.back();
    }
    const std::string def = SwitchDefaultOf(*g, *sw);  // 13-68 v3 review: what the Switch plays unset
    // The animation's value: the one it has, else the one named after it, else a new one. 13-68 v3 review: never one
    // an item outside its pool holds (`held_elsewhere`: that animation's layers are not this one's).
    std::string value;
    for (const ValueSet& v : item_values)
        if (v.group_uid == g->uid && v.value_uid != def && !held_elsewhere.count(v.value_uid))  // 13-68 v3
            for (const SwitchValue& x : g->values)
                if (x.uid == v.value_uid) value = x.uid;
    for (const SwitchValue& x : g->values)
        if (value.empty() && x.uid != def && x.name == anim_name && !held_elsewhere.count(x.uid)) value = x.uid;  // 13-68 v3
    if (value.empty()) {
        SwitchValue v;
        v.uid = new_uid();
        // 13-68 v3 review: a name of its own ("Walk", "Walk 2"...): another animation's value may hold it
        const std::string base = anim_name.empty() ? std::string("Animation") : anim_name;
        v.name = base;
        for (int k = 2;; ++k) {
            const bool used = std::any_of(g->values.begin(), g->values.end(), [&](const SwitchValue& x) { return x.name == v.name; });
            if (!used) break;
            v.name = base + " " + std::to_string(k);
        }
        g->values.push_back(v);
        value = v.uid;
    }
    // Its Blend under the Switch (the animation's layers), made on its first layer.
    std::string anim_blend;
    for (const SwitchCase& c : sw->cases)
        if (c.value_uid == value && FindNode(*lib, c.child_uid)) anim_blend = c.child_uid;
    if (anim_blend.empty()) {
        SoundNode b;
        b.uid = new_uid();
        b.parent = sw->uid;
        b.type = SoundNodeType::Blend;
        b.name = FindValueName(*g, value);  // 13-68 v3 review: its value's name ("Walk 2")
        anim_blend = b.uid;
        std::vector<SwitchCase>& cases = sw->cases;
        cases.erase(std::remove_if(cases.begin(), cases.end(), [&](const SwitchCase& c) { return c.value_uid == value; }), cases.end());
        SwitchCase c;
        c.value_uid = value;
        c.child_uid = b.uid;
        cases.push_back(c);
        lib->nodes.push_back(b);  // `sw` may move: not used after this
    }
    layer[0].parent = anim_blend;
    layer[0].offset_ms = offset_ms;
    layer[0].layer_keep_offset = true;
    for (SoundNode& n : layer) lib->nodes.push_back(std::move(n));
    out.group_uid = g->uid;
    out.value_uid = value;
    return out;
}

bool SeveralStackEvents(const std::vector<PlacedTag>& tags)
{
    for (size_t i = 1; i < tags.size(); ++i)
        if (GuidUpper(tags[i].owner) != GuidUpper(tags[0].owner) || tags[i].rid != tags[0].rid ||
            std::fabs(tags[i].t - tags[0].t) > kEventMatchSeconds)
            return true;
    return false;
}

bool IsStackItem(const SoundLibrary& lib, const std::string& guid, bool placed_for_library, bool child_in_stack)
{
    if (placed_for_library || child_in_stack) return true;
    if (guid.empty()) return false;
    for (const SoundNode& n : lib.nodes)
        if (n.type == SoundNodeType::Sound && n.sound.source.kind == SoundSourceKind::Item &&
            GuidUpper(n.sound.source.item_guid) == GuidUpper(guid))
            return true;
    return false;
}

std::string StackLayerName(const SoundLibrary& lib)
{
    const SoundNode* root = lib.root.empty() ? nullptr : FindNode(lib, lib.root);
    int              k = 1;
    if (root && root->type == SoundNodeType::Blend) {
        k = 0;
        for (const SoundNode& n : lib.nodes) k += n.parent == root->uid ? 1 : 0;
    }
    return "Layer " + std::to_string(k + 1);
}

double StackLayerOffset(double anchor, const std::vector<double>& markers)
{
    double best = 0.0, best_d = -1.0;
    for (double m : markers) {
        const double d = std::fabs(anchor - m);
        if (best_d < 0.0 || d < best_d - 1e-12 || (std::fabs(d - best_d) <= 1e-12 && anchor - m > best)) {
            best_d = d;
            best = anchor - m;
        }
    }
    return best_d < 0.0 ? 0.0 : best;
}

std::vector<SoundDraw> PlayedDraws(const SoundLibrary& lib, const std::vector<std::pair<std::string, int>>& played)
{
    std::vector<SoundDraw> out;
    auto add = [&out](const std::string& node, int pick) {
        for (const SoundDraw& d : out)
            if (d.node == node) return;
        SoundDraw d;
        d.node = node;
        d.pick = pick;
        out.push_back(d);
    };
    for (const SoundNode& n : lib.nodes) {
        if (n.type != SoundNodeType::Sound || n.sound.source.kind != SoundSourceKind::Item) continue;
        const auto hit = std::find_if(played.begin(), played.end(), [&](const std::pair<std::string, int>& p) {
            const std::string& a = p.first;
            const std::string& b = n.sound.source.item_guid;
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
                   });
        });
        if (hit == played.end()) continue;
        if (hit->second >= 0) add(n.uid, hit->second);
        std::string child = n.uid;
        size_t guard = 0;  // freeze audit #6: a parent loop (a damaged store) ends the walk
        for (const SoundNode* up = FindNode(lib, n.parent); up && guard <= lib.nodes.size(); up = FindNode(lib, up->parent), ++guard) {
            if (up->type == SoundNodeType::Random) {
                const std::vector<const SoundNode*> kids = ChildrenOf(lib, up->uid);
                for (size_t k = 0; k < kids.size(); ++k)
                    if (kids[k]->uid == child) add(up->uid, static_cast<int>(k));
            }
            child = up->uid;
            if (up->parent.empty()) break;
        }
    }
    return out;
}

std::vector<double> SourceZonePositions(double project_end, const std::vector<double>& positions)
{
    std::vector<double> out;
    if (positions.empty()) return out;
    const double first = *std::min_element(positions.begin(), positions.end());
    for (double p : positions) out.push_back(PromoteCopyPosition(project_end, first, p));
    return out;
}

namespace {

std::string Count(int n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

int NearestPile(const std::vector<AttachItem>& items, const std::vector<std::vector<int>>& piles, double t)
{
    int    best = -1;
    double best_d = 0.0;
    for (size_t k = 0; k < piles.size(); ++k) {
        const int top = TopmostOf(items, piles[k]);
        if (top < 0) continue;
        const double d = std::fabs(PileAnchor(items[static_cast<size_t>(top)]) - t);
        if (best < 0 || d < best_d) {
            best = static_cast<int>(k);
            best_d = d;
        }
    }
    return best;
}

std::string SpreadRunReport(const SpreadRunCounts& c)
{
    std::string out;
    if (c.selected_markers > 1)
        out += "Used " + (c.marker.empty() ? c.type : c.marker) + " (" + std::to_string(c.selected_markers) + " markers selected)\n";
    out += c.type + (c.all_instances ? " (all animations with this marker): " : " (this animation): ");
    std::vector<std::string> parts;
    std::string              lib = c.add_layer ? "layer added to library " + c.type + (c.all_instances ? "" : " for this animation")
                                               : std::string(c.reused ? "library " : "new library ") + c.type;
    lib += ", " + Count(c.variations, "variation", "variations");
    if (c.layers != c.variations) lib += " (" + Count(c.layers, "item", "items") + ")";
    parts.push_back(lib);
    parts.push_back(Count(c.animations, "animation", "animations"));
    if (c.placed > 0) parts.push_back(Count(c.placed, "sound placed", "sounds placed"));
    if (c.replaced > 0) parts.push_back(std::to_string(c.replaced) + " changed");
    if (c.kept > 0) parts.push_back(std::to_string(c.kept) + " already in place");
    if (c.to_source > 0) parts.push_back(Count(c.to_source, "earlier sound to the source zone", "earlier sounds to the source zone"));
    if (c.events == 0) parts.push_back("no marker to place on");
    if (!c.skip_note.empty()) parts.push_back(c.skip_note);
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
    return out;
}

}  // namespace rav
