// SPDX-License-Identifier: MIT
//
// See sound_spread_plan.h (13-68 v2).

#include "sound_spread_plan.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

SoundNode SoundOf(const SpreadSource& s, const std::string& uid, const std::string& parent)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = SoundNodeType::Sound;
    n.name = s.name.empty() ? std::string("Sound") : s.name;
    n.sound = s.sound;
    SyncFromSnap(n.sound, s.facts.snap);
    return n;
}

}  // namespace

SoundData PileSoundData(const std::string& folder_guid, int takes, double len)
{
    SoundData d;
    d.source.kind = SoundSourceKind::Item;
    d.source.item_guid = folder_guid;
    d.linked_copies = true;
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

void AddStackLayer(SoundLibrary& lib, std::vector<SoundNode> layer, double offset_ms, const std::string& blend_uid)
{
    if (layer.empty()) return;
    const SoundNode* root = lib.root.empty() ? nullptr : FindNode(lib, lib.root);
    if (!root) {
        layer[0].offset_ms = 0.0;
        SetLibraryContent(lib, std::move(layer));
        return;
    }
    std::string blend = lib.root;
    if (root->type != SoundNodeType::Blend) {
        SoundNode b;
        b.uid = blend_uid;
        b.type = SoundNodeType::Blend;
        b.name = lib.name;
        if (SoundNode* old = FindNode(lib, lib.root)) {
            old->parent = b.uid;
            old->offset_ms = 0.0;
            old->layer_keep_offset = true;
        }
        lib.nodes.insert(lib.nodes.begin(), b);
        lib.root = b.uid;
        blend = b.uid;
    }
    layer[0].parent = blend;
    layer[0].offset_ms = offset_ms;
    layer[0].layer_keep_offset = true;
    for (SoundNode& n : layer) lib.nodes.push_back(std::move(n));
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

std::string SpreadRunReport(const SpreadRunCounts& c)
{
    std::string out;
    if (c.selected_markers > 1)
        out += "Used " + c.type + " (" + std::to_string(c.selected_markers) + " markers selected)\n";
    out += c.type + (c.all_instances ? " (all instances): " : " (this animation): ");
    std::vector<std::string> parts;
    std::string              lib = c.add_layer ? "layer added to library " + c.type
                                               : std::string(c.reused ? "library " : "new library ") + c.type;
    lib += ", " + Count(c.variations, "variation", "variations");
    if (c.layers != c.variations) lib += " (" + Count(c.layers, "item", "items") + ")";
    parts.push_back(lib);
    parts.push_back(Count(c.animations, "animation", "animations"));
    if (c.placed > 0) parts.push_back(Count(c.placed, "sound placed", "sounds placed"));
    if (c.replaced > 0) parts.push_back(std::to_string(c.replaced) + " changed");
    if (c.kept > 0) parts.push_back(std::to_string(c.kept) + " already in place");
    if (c.events == 0) parts.push_back("no marker to place on");
    if (!c.skip_note.empty()) parts.push_back(c.skip_note);
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
    return out;
}

}  // namespace rav
