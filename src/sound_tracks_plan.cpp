// SPDX-License-Identifier: MIT
//
// See sound_tracks_plan.h (stories 13-13, 13-16, 13-17, 13-39).

#include "sound_tracks_plan.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "ravlib.h"
#include "sound_record.h"

namespace rav {

namespace {

constexpr const char* kUnnamed = "(unnamed)";

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

// ---- Story 13-39: the Track template -------------------------------------------------------------

bool ParseTemplateLevel(const std::string& word, TemplateLevel* out)
{
    TemplateLevel l;
    if (word == "lib") l.kind = LevelKind::Library;
    else if (word == "blend") l.kind = LevelKind::Blend;
    else if (word == "layer") l.kind = LevelKind::Layer;
    else if (word == "end") l.kind = LevelKind::End;
    else if (word.size() > 2 && word.compare(0, 2, "g:") == 0) {
        l.kind = LevelKind::Group;
        l.arg = word.substr(2);
    } else if (word.size() > 2 && word.compare(0, 2, "c:") == 0) {
        l.kind = LevelKind::Custom;
        l.arg = word.substr(2);
    } else {
        return false;
    }
    if (out) *out = l;
    return true;
}

std::string TemplateLevelWord(const TemplateLevel& level)
{
    switch (level.kind) {
    case LevelKind::Library: return "lib";
    case LevelKind::Group: return "g:" + level.arg;
    case LevelKind::Blend: return "blend";
    case LevelKind::Layer: return "layer";
    case LevelKind::Custom: return "c:" + level.arg;
    case LevelKind::End: break;
    }
    return "end";
}

std::vector<std::string> CleanTemplate(const std::vector<std::string>& words)
{
    std::vector<std::string> out;
    bool                     end = false;
    for (const std::string& w : words) {
        TemplateLevel l;
        if (!ParseTemplateLevel(w, &l)) continue;
        if (l.kind == LevelKind::End) {
            if (end) continue;
            end = true;
        }
        out.push_back(w);
    }
    if (!end) out.push_back("end");
    return out;
}

namespace {

// The library's nodes from the root down, depth-first in child order (each once).
void TreeOrder(const SoundLibrary& lib, const std::string& uid, std::vector<const SoundNode*>* out,
               std::set<std::string>* seen)
{
    if (!seen->insert(uid).second) return;
    const SoundNode* n = FindNode(lib, uid);
    if (!n) return;
    out->push_back(n);
    for (const SoundNode* c : ChildrenOf(lib, uid)) TreeOrder(lib, c->uid, out, seen);
}

std::vector<const SoundNode*> NodesInTreeOrder(const SoundLibrary& lib)
{
    std::vector<const SoundNode*> out;
    std::set<std::string>         seen;
    if (!lib.root.empty()) TreeOrder(lib, lib.root, &out, &seen);
    for (const SoundNode& n : lib.nodes)  // a root left unset, or nodes cut off it: still in line order
        if (n.parent.empty() && !seen.count(n.uid)) TreeOrder(lib, n.uid, &out, &seen);
    return out;
}

// A key's text: '%', '|', ',', '>', ':' and control characters percent-encoded (a mark field never splits).
std::string KeyText(const std::string& s)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string       out;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F || c == '%' || c == '|' || c == ',' || c == '>' || c == ':') {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        } else {
            out += ch;
        }
    }
    return out;
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string UnKeyText(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int a = HexDigit(s[i + 1]), b = HexDigit(s[i + 2]);
            if (a >= 0 && b >= 0) {
                out += static_cast<char>(a * 16 + b);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// One level's value for a Sound: its name and key; false when it has none (skipped for it).
// `group_seen` counts the group levels met so far per group (the n-th takes the n-th step).
bool LevelValue(const TemplateLevel& l, const std::vector<SwitchStep>& path, const BlendLayer& layer,
                const std::string& library_name, std::map<std::string, int>* group_seen, std::string* name,
                std::string* key)
{
    switch (l.kind) {
    case LevelKind::Library:
        *name = Named(library_name);
        *key = "l";
        return true;
    case LevelKind::Group: {
        const int want = (*group_seen)[l.arg]++;
        int       n = 0;
        for (const SwitchStep& s : path) {
            if (s.group_uid != l.arg) continue;
            if (n++ < want) continue;
            *name = Named(s.value_name);
            *key = "g" + s.group_uid + ":" + s.value_uid;
            return true;
        }
        return false;
    }
    case LevelKind::Blend:
        if (!layer.in_blend) return false;
        *name = layer.blend_name;
        *key = "b" + layer.blend_uid;
        return true;
    case LevelKind::Layer:
        if (!layer.in_blend) return false;
        *name = layer.layer_name;
        *key = "y" + layer.layer_uid;
        return true;
    case LevelKind::Custom: {
        const std::string t = Trimmed(l.arg);
        if (t.empty()) return false;
        *name = t;
        *key = "c" + KeyText(t);
        return true;
    }
    case LevelKind::End: break;
    }
    return false;
}

}  // namespace

std::map<std::string, int> TemplateGroupRepeats(const SoundLibrary& lib)
{
    std::map<std::string, int> out;
    for (const SoundNode& n : lib.nodes) {
        if (n.type != SoundNodeType::Switch || n.group_uid.empty()) continue;
        int              k = 0;  // the Switches of this group from it up to the root
        const SoundNode* p = &n;
        for (size_t guard = 0; p && guard <= lib.nodes.size(); ++guard) {
            if (p->type == SoundNodeType::Switch && p->group_uid == n.group_uid) ++k;
            p = p->parent.empty() ? nullptr : FindNode(lib, p->parent);
        }
        int& m = out[n.group_uid];
        m = std::max(m, k);
    }
    return out;
}

std::vector<std::string> DefaultTrackTemplate(const SoundStore& store, const SoundLibrary& lib)
{
    (void)store;
    std::vector<std::string>         out = {"lib"};
    std::map<std::string, int>       have;
    bool                             layer = false;
    for (const SoundNode* n : NodesInTreeOrder(lib)) {
        if (n->type == SoundNodeType::Switch && !n->group_uid.empty()) {
            // Its group's k-th Switch from the root (a group nested k times on one path appears k times),
            // in the order the tree first reaches each.
            int              k = 0;
            const SoundNode* p = n;
            for (size_t guard = 0; p && guard <= lib.nodes.size(); ++guard) {
                if (p->type == SoundNodeType::Switch && p->group_uid == n->group_uid) ++k;
                p = p->parent.empty() ? nullptr : FindNode(lib, p->parent);
            }
            for (int& h = have[n->group_uid]; h < k; ++h) out.push_back("g:" + n->group_uid);
        }
        if (n->type == SoundNodeType::Sound && LayerOf(lib, n->uid).in_blend) layer = true;
    }
    if (layer) out.push_back("layer");  // T1: no Blend level by default (the user may add it)
    out.push_back("end");
    return out;
}

std::vector<std::string> TrackTemplateOf(const SoundStore& store, const SoundLibrary& lib)
{
    return lib.track_levels.empty() ? DefaultTrackTemplate(store, lib) : CleanTemplate(lib.track_levels);
}

bool CanAddTemplateLevel(const SoundLibrary& lib, const std::vector<std::string>& words, const std::string& word)
{
    TemplateLevel l;
    if (!ParseTemplateLevel(word, &l) || l.kind == LevelKind::End) return false;
    if (l.kind == LevelKind::Custom) return !Trimmed(l.arg).empty();
    const int have = static_cast<int>(std::count(words.begin(), words.end(), word));
    if (l.kind != LevelKind::Group) return have == 0;
    const std::map<std::string, int> repeats = TemplateGroupRepeats(lib);
    auto                             r = repeats.find(l.arg);
    return have < std::max(1, r == repeats.end() ? 0 : r->second);
}

std::vector<TrackStep> TemplateTrackPath(const std::vector<std::string>& words, const std::vector<SwitchStep>& path,
                                         const BlendLayer& layer, const std::string& lands_in, const std::string& lib_uid,
                                         const std::string& library_name, const std::string& sep)
{
    const std::vector<std::string> tpl = CleanTemplate(words);
    const std::string              prefix = "T|" + lands_in + "|" + lib_uid + "|";
    std::vector<TrackStep>         out;
    out.push_back({Named(lands_in), true, ""});
    std::map<std::string, int> group_seen;
    std::string                folder_keys, joined_name, joined_keys;
    bool                       after_end = false, joined = false;
    for (const std::string& w : tpl) {
        TemplateLevel l;
        if (!ParseTemplateLevel(w, &l)) continue;
        if (l.kind == LevelKind::End) {
            after_end = true;
            continue;
        }
        std::string name, key;
        if (!LevelValue(l, path, layer, library_name, &group_seen, &name, &key)) continue;
        if (!after_end) {
            folder_keys += (folder_keys.empty() ? "" : ",") + key;
            out.push_back({name, true, prefix + folder_keys});
        } else {
            joined_name += (joined ? sep : std::string()) + name;
            joined_keys += (joined ? "," : "") + key;
            joined = true;
        }
    }
    if (joined) out.push_back({joined_name, false, prefix + folder_keys + ">" + joined_keys});
    if (out.size() == 1) return {};  // no level gives a value: 13-11's "layer N" track
    out.back().folder = false;
    return out;
}

std::vector<TrackStep> SoundTemplatePath(const SoundStore& store, const SoundLibrary& lib, const std::string& sound_uid,
                                         const std::vector<std::string>& words)
{
    const std::vector<SwitchStep> path = SwitchPathOf(store, lib, sound_uid);
    const BlendLayer              layer = LayerOf(lib, sound_uid);
    return TemplateTrackPath(words, path, layer, EffectiveLandsIn(lib, sound_uid), lib.uid, lib.name,
                             TrackSeparatorOf(store));
}

std::vector<TrackStep> SoundTemplatePath(const SoundStore& store, const SoundLibrary& lib, const std::string& sound_uid)
{
    return SoundTemplatePath(store, lib, sound_uid, TrackTemplateOf(store, lib));
}

namespace {

// Applies `edit` to the library's template (its tree order when none is stored); stores none when the
// result is the tree order. True when the stored template changed.
bool EditTemplate(SoundStore& store, const std::string& lib_uid, const std::function<bool(std::vector<std::string>&)>& edit)
{
    SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib) return false;
    std::vector<std::string> words = TrackTemplateOf(store, *lib);
    if (!edit(words)) return false;
    words = CleanTemplate(words);
    if (words == DefaultTrackTemplate(store, *lib)) words.clear();
    if (words == lib->track_levels) return false;
    lib->track_levels = words;
    return true;
}

}  // namespace

bool MoveTemplateLevel(SoundStore& store, const std::string& lib_uid, int from, int to)
{
    return EditTemplate(store, lib_uid, [from, to](std::vector<std::string>& w) {
        const int n = static_cast<int>(w.size());
        if (from < 0 || to < 0 || from >= n || to >= n || from == to) return false;
        const std::string moved = w[static_cast<size_t>(from)];
        w.erase(w.begin() + from);
        w.insert(w.begin() + to, moved);
        return true;
    });
}

bool AddTemplateLevel(SoundStore& store, const std::string& lib_uid, const std::string& word)
{
    TemplateLevel l;
    if (!ParseTemplateLevel(word, &l) || l.kind == LevelKind::End) return false;
    std::string add = word;
    if (l.kind == LevelKind::Custom) {
        l.arg = Trimmed(l.arg);
        if (l.arg.empty()) return false;
        add = TemplateLevelWord(l);
    }
    const SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib) return false;
    const SoundLibrary& L = *lib;
    return EditTemplate(store, lib_uid, [&](std::vector<std::string>& w) {
        if (l.kind == LevelKind::Custom) {
            w.insert(w.begin(), add);  // the user's folders usually sit on top; dragged elsewhere
            return true;
        }
        if (!CanAddTemplateLevel(L, w, add)) return false;
        auto end = std::find(w.begin(), w.end(), std::string("end"));
        w.insert(end, add);
        return true;
    });
}

bool RemoveTemplateLevel(SoundStore& store, const std::string& lib_uid, int index)
{
    return EditTemplate(store, lib_uid, [index](std::vector<std::string>& w) {
        if (index < 0 || index >= static_cast<int>(w.size()) || w[static_cast<size_t>(index)] == "end") return false;
        w.erase(w.begin() + index);
        return true;
    });
}

bool ResetTrackTemplate(SoundStore& store, const std::string& lib_uid)
{
    SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib || lib->track_levels.empty()) return false;
    lib->track_levels.clear();
    return true;
}

std::vector<StructureRow> TemplateStructure(const SoundStore& store, const SoundLibrary& lib)
{
    struct TrieNode {
        StructureRow     row;
        std::vector<int> kids;
    };
    std::vector<TrieNode>      nodes(1);  // [0] = the top
    std::map<std::string, int> by_key;    // parent index + key -> node
    const std::vector<std::string> words = TrackTemplateOf(store, lib);
    for (const SoundNode* n : NodesInTreeOrder(lib)) {
        if (n->type != SoundNodeType::Sound) continue;
        const std::vector<TrackStep> path = SoundTemplatePath(store, lib, n->uid, words);
        int                          cur = 0;
        for (size_t i = 0; i < path.size(); ++i) {
            const std::string key = std::to_string(cur) + "\x1f" + (path[i].mark.empty() ? "\x1e" + path[i].name : path[i].mark);
            auto              hit = by_key.find(key);
            int               at = 0;
            if (hit == by_key.end()) {
                at = static_cast<int>(nodes.size());
                TrieNode t;
                t.row.depth = static_cast<int>(i);
                t.row.name = path[i].name;
                t.row.mark = path[i].mark;
                t.row.why = path[i].mark.empty() ? std::string("Lands in") : MarkLevelLabel(store, path[i].mark);
                nodes.push_back(t);
                nodes[static_cast<size_t>(cur)].kids.push_back(at);
                by_key.emplace(key, at);
            } else {
                at = hit->second;
            }
            nodes[static_cast<size_t>(at)].row.sounds.push_back(n->uid);
            if (i + 1 == path.size()) nodes[static_cast<size_t>(at)].row.holds = true;
            cur = at;
        }
    }
    std::vector<StructureRow> out;
    std::function<void(int)>  walk = [&](int i) {
        for (int k : nodes[static_cast<size_t>(i)].kids) {
            StructureRow r = nodes[static_cast<size_t>(k)].row;
            r.folder = !nodes[static_cast<size_t>(k)].kids.empty();
            out.push_back(r);
            walk(k);
        }
    };
    walk(0);
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

// The library uid of a template mark ("" when it is none).
std::string LibUidOf(const std::string& mark)
{
    std::string uid;
    if (StartsWith(mark, "T|")) return FieldFromEnd(mark, 1, &uid) ? uid : std::string();
    return {};
}

std::string ValueName(const SoundStore& store, const std::string& group_uid, const std::string& value_uid)
{
    if (const SwitchGroup* g = FindGroup(store, group_uid))
        for (const SwitchValue& v : g->values)
            if (v.uid == value_uid) return Named(v.name);
    return {};  // a deleted group or value: no Library name
}

// The name of one template key (TemplateTrackPath), "" when it names nothing any more.
std::string KeyName(const SoundStore& store, const SoundLibrary& lib, const std::string& key)
{
    if (key.empty()) return {};
    const std::string rest = key.substr(1);
    switch (key[0]) {
    case 'l': return rest.empty() ? Named(lib.name) : std::string();
    case 'g': {
        const size_t colon = rest.find(':');
        return colon == std::string::npos ? std::string() : ValueName(store, rest.substr(0, colon), rest.substr(colon + 1));
    }
    case 'b': {
        const SoundNode* b = FindNode(lib, rest);
        return b ? Named(b->name) : std::string();
    }
    case 'y': {
        const SoundNode* n = FindNode(lib, rest);
        if (!n) return {};
        const BlendLayer l = LayerOf(lib, rest);
        return l.in_blend && l.layer_uid == rest ? l.layer_name : Named(Trimmed(n->name));
    }
    case 'c': return UnKeyText(rest);
    default: break;
    }
    return {};
}

std::vector<std::string> SplitKeys(const std::string& s)
{
    std::vector<std::string> out;
    size_t                   at = 0;
    while (at < s.size()) {
        size_t comma = s.find(',', at);
        if (comma == std::string::npos) comma = s.size();
        out.push_back(s.substr(at, comma - at));
        at = comma + 1;
    }
    return out;
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
    if (!StartsWith(mark, "T|")) return {};  // a 13-13 / 13-16 mark (before 13-39) or a foreign one
    // An NVK pile child: its source child track's name (a template mark's keys hold no '|').
    const size_t kpos = mark.rfind("|K|");
    if (kpos != std::string::npos && mark.find('|', kpos + 3) == std::string::npos)
        return track_name ? track_name(mark.substr(kpos + 3)) : std::string();
    const SoundLibrary* lib = FindLibrary(store, LibUidOf(mark));
    std::string         keys;
    if (!lib || !FieldFromEnd(mark, 0, &keys)) return {};
    const size_t gt = keys.find('>');
    if (gt == std::string::npos) {  // a folder: its last level's value
        const std::vector<std::string> k = SplitKeys(keys);
        return k.empty() ? std::string() : KeyName(store, *lib, k.back());
    }
    const std::string sep = TrackSeparatorOf(store);
    std::string       joined;
    bool              any = false;
    for (const std::string& k : SplitKeys(keys.substr(gt + 1))) {
        const std::string v = KeyName(store, *lib, k);
        if (v.empty()) return {};  // the track keeps its name
        joined += (any ? sep : std::string()) + v;
        any = true;
    }
    return joined;
}

namespace {

std::string KeyLabel(const SoundStore& store, const std::string& key)
{
    if (key.empty()) return {};
    switch (key[0]) {
    case 'l': return "library";
    case 'g': {
        const size_t       colon = key.find(':');
        const SwitchGroup* g = FindGroup(store, key.substr(1, colon == std::string::npos ? std::string::npos : colon - 1));
        return g ? (g->rav_owned ? "RAV - " : "") + Named(g->name) : std::string("?");  // as its chip
    }
    case 'b': return "Blend";
    case 'y': return "Layer";
    case 'c': return "your folder";
    default: break;
    }
    return {};
}

}  // namespace

std::string MarkLevelLabel(const SoundStore& store, const std::string& mark)
{
    std::string keys;
    if (!StartsWith(mark, "T|") || !FieldFromEnd(mark, 0, &keys)) return {};
    const size_t gt = keys.find('>');
    if (gt == std::string::npos) {
        const std::vector<std::string> k = SplitKeys(keys);
        return k.empty() ? std::string() : KeyLabel(store, k.back());
    }
    std::string out = "name: ";
    bool        any = false;
    for (const std::string& k : SplitKeys(keys.substr(gt + 1))) {
        out += (any ? " + " : "") + KeyLabel(store, k);
        any = true;
    }
    return out;
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
