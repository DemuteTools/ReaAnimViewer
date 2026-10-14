// SPDX-License-Identifier: MIT
//
// See sound_tracks_plan.h (story 13-13).

#include "sound_tracks_plan.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "ravlib.h"
#include "sound_record.h"

namespace rav {

namespace {

constexpr const char* kTrackKey = "trk=";
constexpr const char* kUnnamed = "(unnamed)";

bool IsTrackToken(const std::string& tok)
{
    return tok.compare(0, 4, kTrackKey) == 0;
}

const char* LayoutWord(TrackLayout l)
{
    return l == TrackLayout::Flat ? "flat" : l == TrackLayout::Tree ? "tree" : "";
}

std::string Named(const std::string& s)
{
    return s.empty() ? std::string(kUnnamed) : s;
}

std::string Trimmed(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::string TrimmedLower(const std::string& s)
{
    std::string out = Trimmed(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}

}  // namespace

TrackLayout TrackLayoutOf(const SoundStore& store)
{
    for (const std::string& tok : store.kept) {
        if (!IsTrackToken(tok)) continue;
        const std::string v = tok.substr(4);
        if (v == "flat") return TrackLayout::Flat;
        if (v == "tree") return TrackLayout::Tree;
        return TrackLayout::Off;
    }
    return TrackLayout::Off;
}

bool SetTrackLayout(SoundStore& store, TrackLayout layout)
{
    if (TrackLayoutOf(store) == layout) return false;
    std::vector<std::string> kept;
    bool                     written = false;
    for (const std::string& tok : store.kept) {
        if (!IsTrackToken(tok)) {
            kept.push_back(tok);
            continue;
        }
        if (layout != TrackLayout::Off && !written) kept.push_back(std::string(kTrackKey) + LayoutWord(layout));
        written = true;
    }
    if (layout != TrackLayout::Off && !written) kept.push_back(std::string(kTrackKey) + LayoutWord(layout));
    store.kept = kept;
    return true;
}

namespace {

constexpr const char* kSepKey = "sep=";

struct SepWord {
    const char* sep;
    const char* word;  // "" = the default (no token)
};
constexpr SepWord kSeps[] = {{"_", ""}, {"-", "dash"}, {".", "dot"}, {" ", "space"}};

bool IsSepToken(const std::string& tok)
{
    return tok.compare(0, 4, kSepKey) == 0;
}

}  // namespace

std::string TrackSeparatorOf(const SoundStore& store)
{
    for (const std::string& tok : store.kept) {
        if (!IsSepToken(tok)) continue;
        const std::string v = tok.substr(4);
        for (const SepWord& w : kSeps)
            if (*w.word && v == w.word) return w.sep;
        return "_";
    }
    return "_";
}

const std::vector<std::string>& TrackSeparators()
{
    static const std::vector<std::string> kAll = {"_", "-", ".", " "};
    return kAll;
}

bool SetTrackSeparator(SoundStore& store, const std::string& sep)
{
    const SepWord* want = nullptr;
    for (const SepWord& w : kSeps)
        if (sep == w.sep) want = &w;
    if (!want || TrackSeparatorOf(store) == sep) return false;
    std::vector<std::string> kept;
    bool                     written = false;
    for (const std::string& tok : store.kept) {
        if (!IsSepToken(tok)) {
            kept.push_back(tok);
            continue;
        }
        if (*want->word && !written) kept.push_back(std::string(kSepKey) + want->word);
        written = true;
    }
    if (*want->word && !written) kept.push_back(std::string(kSepKey) + want->word);
    store.kept = kept;
    return true;
}

std::string NumberedName(const std::string& base, int n)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d", n < 0 ? 0 : n);
    return base + buf;
}

std::vector<SwitchStep> SwitchPathOf(const SoundStore& store, const SoundLibrary& lib, const std::string& sound_uid)
{
    std::vector<SwitchStep> up;  // bottom-up
    const SoundNode*        n = FindNode(lib, sound_uid);
    size_t                  guard = 0;
    for (; n && !n->parent.empty() && guard <= lib.nodes.size(); ++guard) {
        const SoundNode* p = FindNode(lib, n->parent);
        if (!p) break;
        if (p->type == SoundNodeType::Switch) {
            const SwitchCase* hit = nullptr;
            for (const SwitchCase& c : p->cases)
                if (c.child_uid == n->uid) {
                    hit = &c;
                    break;
                }
            // On no value, or cased but also Unassigned (CaseChild: no content): never played
            // through this Switch.
            if (!hit || std::find(p->unassigned.begin(), p->unassigned.end(), n->uid) != p->unassigned.end())
                return {};
            SwitchStep s;
            s.group_uid = p->group_uid;
            s.value_uid = hit->value_uid;
            s.group_name = "?";
            s.value_name = "?";
            if (const SwitchGroup* g = FindGroup(store, p->group_uid)) {
                s.group_name = g->name;
                for (const SwitchValue& v : g->values)
                    if (v.uid == hit->value_uid) s.value_name = v.name;
            }
            up.push_back(s);
        }
        n = p;
    }
    if (guard > lib.nodes.size()) return {};  // a cycle
    std::reverse(up.begin(), up.end());
    return up;
}

int MatchChildTrack(const std::vector<std::string>& child_names, const std::string& name)
{
    const std::string want = TrimmedLower(name);
    for (size_t i = 0; i < child_names.size(); ++i)
        if (TrimmedLower(child_names[i]) == want) return static_cast<int>(i);
    return -1;
}

int PickLayerChild(const std::vector<std::string>& child_names, const std::vector<bool>& switch_tree, int layer,
                   int* next_number)
{
    auto is_tree = [&](size_t i) { return i < switch_tree.size() && switch_tree[i]; };
    std::vector<std::string> names;  // the layer candidates
    std::vector<int>         index;  // their index among all the children
    for (size_t i = 0; i < child_names.size(); ++i)
        if (!is_tree(i)) {
            names.push_back(child_names[i]);
            index.push_back(static_cast<int>(i));
        }
    if (next_number) *next_number = static_cast<int>(names.size()) + 1;
    // By position among the children that are not Switch trees, as at 1fe0956 (a name match
    // could give two layers the same track once a "layer K" track was deleted or moved).
    if (layer < 1) return -1;
    if (static_cast<int>(names.size()) >= layer) return index[static_cast<size_t>(layer - 1)];
    return -1;
}

// ---- Story 13-16 ---------------------------------------------------------------------------

namespace {

// The flat layers of a Blend: its children in order, a Blend child's own flat layers in its place.
void FlatLayers(const SoundLibrary& lib, const std::string& blend_uid, std::vector<const SoundNode*>* out,
                std::set<std::string>* seen)
{
    if (!seen->insert(blend_uid).second) return;  // a cycle
    for (const SoundNode* c : ChildrenOf(lib, blend_uid)) {
        if (c->type == SoundNodeType::Blend) FlatLayers(lib, c->uid, out, seen);
        else out->push_back(c);
    }
}

std::string PathMark(const char* kind, const std::string& lands_in, const std::string& lib_uid,
                     const std::vector<SwitchStep>& path, size_t levels)
{
    std::string m = std::string(kind) + "|" + lands_in + "|" + lib_uid + "|";
    for (size_t i = 0; i < levels && i < path.size(); ++i) {
        if (i > 0) m += ',';
        m += path[i].group_uid + ":" + path[i].value_uid;
    }
    return m;
}

constexpr const char* kLaneTag = "|S|";

}  // namespace

BlendLayer LayerOf(const SoundLibrary& lib, const std::string& sound_uid)
{
    BlendLayer  out;
    std::string nearest, child;
    if (!BlendLayerPath(lib, sound_uid, &nearest, &child)) return out;
    // The folder: the outermost Blend of a chain of Blends nested straight in Blends.
    std::string top = nearest;
    for (size_t guard = 0; guard <= lib.nodes.size(); ++guard) {
        const SoundNode* b = FindNode(lib, top);
        const SoundNode* p = b && !b->parent.empty() ? FindNode(lib, b->parent) : nullptr;
        if (!p || p->type != SoundNodeType::Blend || p->uid == nearest) break;
        top = p->uid;
    }
    const SoundNode* top_node = FindNode(lib, top);
    const SoundNode* layer = FindNode(lib, child);
    if (!top_node || !layer) return out;
    out.in_blend = true;
    out.blend_uid = top;
    out.blend_name = Named(top_node->name);
    out.layer_uid = child;
    // "Thud", "Thud 2": the layers before it in the flat order that share its name.
    std::vector<const SoundNode*> flat;
    std::set<std::string>         seen;
    FlatLayers(lib, top, &flat, &seen);
    const std::string mine = TrimmedLower(Named(layer->name));
    int               same = 0;
    for (const SoundNode* f : flat) {
        if (f->uid == child) break;
        if (TrimmedLower(Named(f->name)) == mine) ++same;
    }
    out.layer_name = Named(Trimmed(layer->name));  // "thud " and "Thud" give "Thud", "thud 2"
    if (same > 0) out.layer_name = NumberedName(out.layer_name, same + 1);  // "Thud02" (13-17 N1)
    return out;
}

std::string LibraryMark(const std::string& lands_in, const std::string& lib_uid)
{
    return "L|" + lands_in + "|" + lib_uid;
}

std::string BlendMark(const std::string& lands_in, const std::string& lib_uid, const std::string& blend_uid)
{
    return "B|" + lands_in + "|" + lib_uid + "|" + blend_uid;
}

std::string LayerMark(const std::string& parent, const std::string& layer_uid)
{
    return parent + "|Y|" + layer_uid;
}

std::string PileChildMark(const std::string& layer_mark, const std::string& source_track_guid)
{
    return layer_mark + "|K|" + source_track_guid;
}

std::string LaneMark(const std::string& base, int k)
{
    return base + kLaneTag + std::to_string(k);
}

std::string GuidTrackMark(const std::string& track_guid)
{
    return "#" + track_guid;
}

int LaneOfMark(const std::string& mark, std::string* base)
{
    if (base) *base = mark;
    const size_t at = mark.rfind(kLaneTag);
    if (at == std::string::npos) return 0;
    const std::string digits = mark.substr(at + 3);
    if (digits.empty() || digits.size() > 6) return 0;
    for (char c : digits)
        if (c < '0' || c > '9') return 0;
    const int k = std::stoi(digits);
    if (k < 1) return 0;
    if (base) *base = mark.substr(0, at);
    return k;
}

std::vector<TrackStep> SoundTrackPath(const std::vector<SwitchStep>& path, const BlendLayer& layer, TrackLayout layout,
                                      const std::string& lands_in, const std::string& lib_uid,
                                      const std::string& library_name, const std::string& sep)
{
    const bool has_path = layout != TrackLayout::Off && !path.empty();
    if (!has_path && !layer.in_blend) return {};
    std::vector<TrackStep> out;
    out.push_back({Named(lands_in), true, ""});
    out.push_back({Named(library_name), true, LibraryMark(lands_in, lib_uid)});
    if (has_path) {
        if (layout == TrackLayout::Flat) {
            std::string joined;
            for (size_t i = 0; i < path.size(); ++i) {
                if (i > 0) joined += sep;
                joined += Named(path[i].value_name);
            }
            out.push_back({joined, false, PathMark("PF", lands_in, lib_uid, path, path.size())});
        } else {
            for (size_t i = 0; i < path.size(); ++i)
                out.push_back({Named(path[i].value_name), i + 1 < path.size(), PathMark("PT", lands_in, lib_uid, path, i + 1)});
        }
        if (layer.in_blend) {
            out.back().folder = true;
            const std::string parent = out.back().mark;
            out.push_back({layer.layer_name, false, LayerMark(parent, layer.layer_uid)});
        }
        return out;
    }
    const std::string blend = BlendMark(lands_in, lib_uid, layer.blend_uid);
    out.push_back({layer.blend_name, true, blend});
    out.push_back({layer.layer_name, false, LayerMark(blend, layer.layer_uid)});
    return out;
}

bool MayCrossfade(const LaneSpan& a, const LaneSpan& b)
{
    constexpr double kTouch = 1e-6;
    const bool       a_first = a.start < b.start || (a.start == b.start && a.end <= b.end);
    const LaneSpan&  x = a_first ? a : b;
    const LaneSpan&  y = a_first ? b : a;
    const double     over = x.end - y.start;
    return over > kTouch && y.end > x.end + kTouch && over <= std::min(x.fade_out, y.fade_in) + kTouch;
}

bool CrossfadeButted(const LaneSpan& a, const LaneSpan& b)
{
    return !a.group.empty() && a.group == b.group && MayCrossfade(a, b);
}

std::string PlacementGroup(const PlacedTag& tag)
{
    if (tag.owner.empty()) return {};
    char t[64];
    std::snprintf(t, sizeof(t), "%.9f", tag.t);
    return tag.owner + "|" + tag.rid + "|" + t + "|" + tag.node;
}

bool SpansOverlap(const LaneSpan& a, const LaneSpan& b)
{
    constexpr double kTouch = 1e-6;
    return a.start < b.end - kTouch && b.start < a.end - kTouch && !CrossfadeButted(a, b);
}

bool FitsLane(const std::vector<LaneSpan>& lane, const LaneSpan& s, int exclude_id)
{
    for (const LaneSpan& o : lane) {
        if (exclude_id >= 0 && o.id == exclude_id) continue;
        if (SpansOverlap(o, s)) return false;
    }
    return true;
}

int PickLane(const std::vector<std::vector<LaneSpan>>& lanes, const LaneSpan& s, int exclude_id)
{
    for (size_t i = 0; i < lanes.size(); ++i)
        if (FitsLane(lanes[i], s, exclude_id)) return static_cast<int>(i);
    return -1;
}

std::vector<int> SpillOnLanes(std::vector<LaneSpan> items, std::vector<std::vector<LaneSpan>>* lanes)
{
    std::vector<int> out(items.size(), -1);
    std::vector<size_t> order(items.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return items[a].start < items[b].start; });
    for (size_t i : order) {
        int l = PickLane(*lanes, items[i]);
        if (l < 0) {
            lanes->emplace_back();
            l = static_cast<int>(lanes->size()) - 1;
        }
        (*lanes)[static_cast<size_t>(l)].push_back(items[i]);
        out[i] = l;
    }
    return out;
}

bool PlanSpill(const std::vector<LaneSpan>& items, const std::vector<std::string>& tks, const std::string& base_mark,
               const LaneSpan& item, SpillPlan* out)
{
    SpillPlan plan;
    plan.lane_of.assign(items.size(), -1);
    if (FitsLane(items, item)) {
        if (out) *out = plan;
        return false;
    }
    std::vector<LaneSpan> movers;
    std::vector<size_t>   from;
    for (size_t i = 0; i < items.size(); ++i)
        if (i < tks.size() && !base_mark.empty() && tks[i] == base_mark) {
            movers.push_back(items[i]);
            from.push_back(i);
        }
    movers.push_back(item);
    std::vector<std::vector<LaneSpan>> lanes;
    const std::vector<int>             at = SpillOnLanes(movers, &lanes);
    for (size_t j = 0; j < from.size(); ++j) plan.lane_of[from[j]] = at[j];
    plan.item_lane = at.back();
    plan.lanes = static_cast<int>(lanes.size());
    if (out) *out = plan;
    return true;
}

int PickLaneK(const std::vector<int>& ks, const std::vector<std::vector<LaneSpan>>& lanes, const LaneSpan& item,
              bool* made)
{
    if (made) *made = false;
    const int l = PickLane(lanes, item);
    if (l >= 0 && static_cast<size_t>(l) < ks.size()) return ks[static_cast<size_t>(l)];
    if (made) *made = true;
    int top = 0;
    for (int k : ks) top = std::max(top, k);
    return top + 1;
}

PlacedSpot PlacedSpotOf(const std::string& tk, bool on_tk_track, std::string* base)
{
    if (base) base->clear();
    if (tk.empty()) return PlacedSpot::NotRav;
    LaneOfMark(tk, base);
    return on_tk_track ? PlacedSpot::OnRavTrack : PlacedSpot::MovedByUser;
}

// ---- Story 13-17 ---------------------------------------------------------------------------

namespace {

// The mark's '|'-separated fields from the end: `back` = 0 the last one. False when too few.
bool FieldFromEnd(const std::string& mark, size_t back, std::string* out)
{
    size_t end = mark.size();
    for (size_t i = 0;; ++i) {
        const size_t bar = end == 0 ? std::string::npos : mark.rfind('|', end - 1);
        if (bar == std::string::npos) return false;
        if (i == back) {
            *out = mark.substr(bar + 1, end - bar - 1);
            return true;
        }
        end = bar;
    }
}

bool StartsWith(const std::string& s, const char* p)
{
    return s.compare(0, std::char_traits<char>::length(p), p) == 0;
}

// The library uid of a library, Switch path or Blend mark ("" when none of those).
std::string LibUidOf(const std::string& mark)
{
    std::string uid;
    if (StartsWith(mark, "L|")) return FieldFromEnd(mark, 0, &uid) ? uid : std::string();
    if (StartsWith(mark, "PF|") || StartsWith(mark, "PT|") || StartsWith(mark, "B|"))
        return FieldFromEnd(mark, 1, &uid) ? uid : std::string();
    return {};
}

std::string ValueName(const SoundStore& store, const std::string& group_uid, const std::string& value_uid)
{
    if (const SwitchGroup* g = FindGroup(store, group_uid))
        for (const SwitchValue& v : g->values)
            if (v.uid == value_uid) return Named(v.name);
    return {};  // a deleted group or value: no Library name
}

}  // namespace

std::string NameForMark(const SoundStore& store, const std::string& mark,
                        const std::function<std::string(const std::string& mark)>& track_name)
{
    if (mark.empty()) return {};
    // A spill child: its base's name, numbered.
    std::string base;
    if (const int k = LaneOfMark(mark, &base)) {
        std::string b = base[0] == '#' ? (track_name ? track_name(base) : std::string()) : NameForMark(store, base, track_name);
        return b.empty() ? std::string() : NumberedName(b, k);
    }
    if (mark[0] == '#') return {};  // a GuidTrackMark: a track RAV does not name (13-11's "layer N")
    // An NVK pile child: its source child track's name.
    const size_t kpos = mark.find("|K|");
    if (kpos != std::string::npos && mark.rfind("|Y|", kpos) != std::string::npos)
        return track_name ? track_name(mark.substr(kpos + 3)) : std::string();
    // A layer track: <parent>|Y|<layer uid>.
    const size_t ypos = mark.rfind("|Y|");
    if (ypos != std::string::npos && mark.find('|', ypos + 3) == std::string::npos) {
        const SoundLibrary* lib = FindLibrary(store, LibUidOf(mark.substr(0, ypos)));
        const std::string   layer_uid = mark.substr(ypos + 3);
        const SoundNode*    n = lib ? FindNode(*lib, layer_uid) : nullptr;
        if (!n) return {};
        const BlendLayer l = LayerOf(*lib, layer_uid);
        return l.in_blend && l.layer_uid == layer_uid ? l.layer_name : Named(Trimmed(n->name));
    }
    const SoundLibrary* lib = FindLibrary(store, LibUidOf(mark));
    if (!lib) return {};
    if (StartsWith(mark, "L|")) return Named(lib->name);
    std::string last;
    if (!FieldFromEnd(mark, 0, &last)) return {};
    if (StartsWith(mark, "B|")) {
        const SoundNode* b = FindNode(*lib, last);
        return b ? Named(b->name) : std::string();
    }
    // A Switch path: "g:v,g:v".
    std::vector<std::string> values;
    size_t                   at = 0;
    while (at <= last.size() && !last.empty()) {
        size_t comma = last.find(',', at);
        if (comma == std::string::npos) comma = last.size();
        const std::string gv = last.substr(at, comma - at);
        const size_t      colon = gv.find(':');
        if (colon == std::string::npos) return {};
        const std::string v = ValueName(store, gv.substr(0, colon), gv.substr(colon + 1));
        if (v.empty()) return {};  // the track keeps its name
        values.push_back(v);
        at = comma + 1;
    }
    if (values.empty()) return {};
    if (StartsWith(mark, "PT|")) return values.back();
    const std::string sep = TrackSeparatorOf(store);
    std::string       joined;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) joined += sep;
        joined += values[i];
    }
    return joined;
}

TrackRename PlanTrackRename(const std::string& cur, const std::string& given, const std::string& want)
{
    TrackRename r;
    if (want.empty()) return r;                    // no Library name: left as it is
    if (!given.empty() && cur != given) return r;  // renamed by the user (N2): never again
    r.rename = cur != want;
    r.record = given != want;
    return r;
}

}  // namespace rav
