// SPDX-License-Identifier: MIT
//
// See sound_spread_plan.h (13-68 v2 / v3).

#include "sound_spread_plan.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
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
    // Antho 2026-10-20: a Sound cut on silence syncs as Silence Sounds do (each region on its own attack), never on the
    // item's snap: each copy plays its own region, never silence.
    if (n.sound.mode != DetectMode::Silence) SyncFromSnap(n.sound, SourceSecondsOf(s.facts.snap, s.playrate));
    return n;
}

}  // namespace

SoundData SilenceVariations(SoundData d, const EntryFacts& facts, const PeakData* peaks)
{
    if (d.mode == DetectMode::Takes || !peaks) return d;
    const SilenceParams           sp;  // what a new Sound gets (-60 dB, 200 ms gap, its pre-roll and tail)
    const std::vector<SoundFrame> frames = DetectFrames(facts, DetectMode::Silence, sp, peaks);
    if (frames.empty()) return d;
    d.mode = DetectMode::Silence;
    d.silence = sp;
    d.frames = frames;
    return d;
}

SoundData PileSoundData(const std::string& folder_guid, int takes, double len, const std::vector<Slice>& slices,
                        const std::vector<SoundFrame>& silence)
{
    SoundData d;
    d.source.kind = SoundSourceKind::Item;
    d.source.item_guid = folder_guid;
    d.linked_copies = true;
    if (takes < 2 && !silence.empty()) {  // Antho 2026-10-20: the top member's regions of sound, take markers ignored
        d.mode = DetectMode::Silence;
        d.frames = silence;
        return d;
    }
    if (slices.size() >= 2) {  // (the Attach passes none since 2026-10-20: its regions of sound instead)  // take markers (NVK): one variation per slice of the top member
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

// 13-68e: the node `top` as a Blend: itself when it is one, else wrapped in a new Blend `blend_uid` named after it (in
// its place: its parent's child, a Switch case naming it, the root). "" when absent.
std::string SlotBlend(SoundLibrary& lib, const std::string& top, const std::string& blend_uid)
{
    if (top.empty() || top == lib.root) return RootBlend(lib, blend_uid);
    SoundNode* t = FindNode(lib, top);
    if (!t) return std::string();
    if (t->type == SoundNodeType::Blend) return top;
    SoundNode b;
    b.uid = blend_uid;
    b.type = SoundNodeType::Blend;
    b.name = t->name;
    b.parent = t->parent;
    b.offset_ms = t->offset_ms;
    b.layer_keep_offset = t->layer_keep_offset;
    if (SoundNode* p = FindNode(lib, t->parent))
        for (SwitchCase& c : p->cases)
            if (c.child_uid == top) c.child_uid = b.uid;
    t->parent = b.uid;
    t->offset_ms = 0.0;
    t->layer_keep_offset = true;
    size_t at = 0;
    while (at < lib.nodes.size() && lib.nodes[at].uid != top) ++at;
    lib.nodes.insert(lib.nodes.begin() + static_cast<long>(at), b);  // `t` may move: not used after this
    return blend_uid;
}

}  // namespace

void AddStackLayer(SoundLibrary& lib, std::vector<SoundNode> layer, double offset_ms, const std::string& blend_uid,
                   const std::string& top)
{
    if (layer.empty()) return;
    if (top.empty() && (lib.root.empty() || !FindNode(lib, lib.root))) {
        layer[0].offset_ms = 0.0;
        SetLibraryContent(lib, std::move(layer));
        return;
    }
    const std::string blend = SlotBlend(lib, top, blend_uid);
    if (blend.empty()) return;
    layer[0].parent = blend;
    layer[0].offset_ms = offset_ms;
    layer[0].layer_keep_offset = true;
    for (SoundNode& n : layer) lib.nodes.push_back(std::move(n));
}


// ---- 13-68e: "<side> only" -----------------------------------------------------------------------------------------

bool IsSideGroup(const SwitchGroup& g)
{
    return g.rav_owned && g.name == kSideGroupName;
}

SwitchGroup* SideGroup(SoundStore& store, const std::function<std::string()>& new_uid)
{
    for (SwitchGroup& g : store.groups)
        if (IsSideGroup(g)) {
            for (const char* side : {"L", "R"})  // a damaged one gets its value back
                if (SideValueOf(g, side[0]).empty()) {
                    SwitchValue v;
                    v.uid = new_uid();
                    v.name = side;
                    g.values.push_back(v);
                }
            return &g;
        }
    SwitchGroup g;
    g.uid = new_uid();
    g.name = kSideGroupName;
    g.rav_owned = true;
    for (const char* side : {"L", "R"}) {
        SwitchValue v;
        v.uid = new_uid();
        v.name = side;
        g.values.push_back(v);
    }
    g.default_value = g.values.front().uid;
    store.groups.push_back(g);
    return &store.groups.back();
}

std::string SideValueOf(const SwitchGroup& g, char side)
{
    const std::string want(1, side);
    for (const SwitchValue& v : g.values)
        if (v.name == want) return v.uid;
    return std::string();
}

bool LibraryUsesSide(const SoundStore& store, const SoundLibrary& lib)
{
    for (const SoundNode& n : lib.nodes)
        if (n.type == SoundNodeType::Switch)
            if (const SwitchGroup* g = FindGroup(store, n.group_uid); g && IsSideGroup(*g)) return true;
    return false;
}

const SwitchGroup* FindSideGroup(const SoundStore& store)
{
    for (const SwitchGroup& g : store.groups)
        if (IsSideGroup(g)) return &g;
    return nullptr;
}

bool FillMissingSideValues(const SoundStore& store, ItemSounds& rec, const std::vector<RuleSide>& rules)
{
    const SwitchGroup* g = FindSideGroup(store);
    if (!g) return false;
    bool changed = false;
    for (const RuleSide& r : rules) {
        if (r.side != 'L' && r.side != 'R') continue;
        RuleSounds* rs = r.rid.empty() ? nullptr : FindRuleSounds(rec, r.rid);
        if (!rs || rs->none || rs->library.empty()) continue;
        const SoundLibrary* lib = FindLibrary(store, rs->library);
        if (!lib || !LibraryUsesSide(store, *lib)) continue;
        const bool has = std::any_of(rs->values.begin(), rs->values.end(), [&](const ValueSet& v) { return v.group_uid == g->uid; });
        const std::string value = SideValueOf(*g, r.side);
        if (has || value.empty()) continue;
        ValueSet v;
        v.group_uid = g->uid;
        v.value_uid = value;
        rs->values.push_back(v);
        changed = true;
    }
    return changed;
}

std::vector<std::string> RemoveSubtree(SoundLibrary& lib, const std::string& uid)
{
    std::vector<std::string> gone;
    if (uid.empty() || !FindNode(lib, uid)) return gone;
    std::set<std::string> in = {uid};
    gone.push_back(uid);
    for (bool grew = true; grew;) {  // the descendants (any order of the lines)
        grew = false;
        for (const SoundNode& n : lib.nodes)
            if (!in.count(n.uid) && in.count(n.parent)) {
                in.insert(n.uid);
                gone.push_back(n.uid);
                grew = true;
            }
    }
    lib.nodes.erase(std::remove_if(lib.nodes.begin(), lib.nodes.end(), [&](const SoundNode& n) { return in.count(n.uid) > 0; }),
                    lib.nodes.end());
    for (SoundNode& n : lib.nodes) {
        n.cases.erase(std::remove_if(n.cases.begin(), n.cases.end(), [&](const SwitchCase& c) { return in.count(c.child_uid) > 0; }),
                      n.cases.end());
        n.unassigned.erase(std::remove_if(n.unassigned.begin(), n.unassigned.end(), [&](const std::string& c) { return in.count(c) > 0; }),
                           n.unassigned.end());
    }
    if (in.count(lib.root)) lib.root.clear();
    return gone;
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
    // 13-68e v5: the scope (Item / Anim / Project) and, in a separated Group, the side written.
    out += c.type + (c.side ? " " + std::string(1, c.side) : std::string()) +
           (c.item ? " (this instance): " : c.all_instances ? " (the project): " : " (this animation): ");
    std::vector<std::string> parts;
    std::string              lib = c.add_layer ? "layer added to library " + c.type + (c.all_instances ? "" : " for this animation")
                                               : std::string(c.reused ? "library " : "new library ") + c.type;
    if (c.item) lib = c.add_layer ? "layer on this instance only" : "on this marker only";
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
