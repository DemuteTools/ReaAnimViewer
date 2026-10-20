// SPDX-License-Identifier: MIT
//
// See library_sync.h. Spec 13-77b.

#include "library_sync.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "library_tree_edit.h"  // UniqueName
#include "sound_record.h"
#include "sound_switch.h"     // SwitchNameKey

namespace rav {

namespace {

// ---- text model: a .ravlib as lines ----------------------------------------------------------------------

struct Line {
    std::string              kind;
    std::vector<std::string> toks;  // "k=v" or a stray token
};

Line ParseLine(const std::string& s)
{
    Line   l;
    size_t pos = 0;
    bool   first = true;
    while (pos <= s.size()) {
        size_t sp = s.find(' ', pos);
        if (sp == std::string::npos) sp = s.size();
        std::string t = s.substr(pos, sp - pos);
        if (first) {
            l.kind = t;
            first = false;
        } else {
            l.toks.push_back(t);
        }
        pos = sp + 1;
    }
    return l;
}

std::string Str(const Line& l)
{
    std::string s = l.kind;
    for (const std::string& t : l.toks) s += ' ' + t;
    return s;
}

size_t EqPos(const std::string& tok)
{
    const size_t e = tok.find('=');
    return e == 0 ? std::string::npos : e;
}

bool GetTok(const Line& l, const std::string& key, std::string* val = nullptr)
{
    for (const std::string& t : l.toks) {
        const size_t e = EqPos(t);
        if (e != std::string::npos && t.compare(0, e, key) == 0 && e == key.size()) {
            if (val) *val = t.substr(e + 1);
            return true;
        }
    }
    return false;
}

std::string TokOr(const Line& l, const std::string& key, const std::string& def = std::string())
{
    std::string v;
    return GetTok(l, key, &v) ? v : def;
}

void EraseTok(Line& l, const std::string& key)
{
    l.toks.erase(std::remove_if(l.toks.begin(), l.toks.end(),
                                [&](const std::string& t) {
                                    const size_t e = EqPos(t);
                                    return (e != std::string::npos && e == key.size() && t.compare(0, e, key) == 0) || t == key;
                                }),
                 l.toks.end());
}

void SetTok(Line& l, const std::string& key, const std::string& val)
{
    const std::string nt = key + '=' + val;
    for (std::string& t : l.toks) {
        const size_t e = EqPos(t);
        if (e != std::string::npos && e == key.size() && t.compare(0, e, key) == 0) {
            t = nt;
            return;
        }
    }
    l.toks.push_back(nt);
}

// Copies one field of `src` into `dst` (a "k=v" token or a stray token); absent in `src` = erased in `dst`.
void CopyTok(Line& dst, const Line& src, const std::string& key)
{
    std::string v;
    if (GetTok(src, key, &v)) {
        SetTok(dst, key, v);
        return;
    }
    const bool stray = std::find(src.toks.begin(), src.toks.end(), key) != src.toks.end();
    EraseTok(dst, key);
    if (stray) dst.toks.push_back(key);
}

// The line as comparable text: tokens sorted, minus `skip` keys.
std::string Canon(const Line& l, const std::vector<std::string>& skip = {})
{
    std::vector<std::string> t;
    for (const std::string& tok : l.toks) {
        const size_t e = EqPos(tok);
        bool         drop = false;
        if (e != std::string::npos)
            for (const std::string& k : skip) drop = drop || (k.size() == e && tok.compare(0, e, k) == 0);
        if (!drop) t.push_back(tok);
    }
    std::sort(t.begin(), t.end());
    std::string s = l.kind;
    for (const std::string& x : t) s += ' ' + x;
    return s;
}

// A line's fields as (key, value); a stray token is (token, "").
std::vector<std::pair<std::string, std::string>> FieldsOf(const Line& l)
{
    std::vector<std::pair<std::string, std::string>> f;
    for (const std::string& t : l.toks) {
        const size_t e = EqPos(t);
        if (e == std::string::npos) f.push_back({t, std::string()});
        else f.push_back({t.substr(0, e), t.substr(e + 1)});
    }
    return f;
}

std::string PctDecode(const std::string& s)
{
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            r += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else {
            r += s[i];
        }
    }
    return r;
}

std::string Num(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

double ToD(const std::string& s)
{
    return std::strtod(s.c_str(), nullptr);
}

struct Var {
    Line              frame;
    std::vector<Line> fz;
};

struct NodeDoc {
    std::string       uid, parent;
    Line              node;
    std::vector<Line> cases, unassigned;
    bool              has_sound = false;
    Line              sound;
    std::vector<Line> parts, zones;
    std::vector<Var>  frames;
    std::vector<Line> parked;  // park / pframe / pzone in order
    std::vector<Line> extra;
};

struct GroupDoc {
    std::string       uid;
    Line              group;
    std::vector<Line> values;
    std::vector<Line> extra;
};

struct Doc {
    std::string           header = "RAVLIB 2";
    std::vector<Line>     pre;
    std::vector<GroupDoc> groups;
    bool                  has_lib = false;
    Line                  lib;
    bool                  has_kit = false;
    Line                  kit;
    std::vector<Line>     lib_extra;
    std::vector<NodeDoc>  nodes;
    std::map<std::string, size_t> idx;

    void Reindex()
    {
        idx.clear();
        for (size_t i = 0; i < nodes.size(); ++i) idx[nodes[i].uid] = i;
    }
    NodeDoc* N(const std::string& uid)
    {
        auto it = idx.find(uid);
        return it == idx.end() ? nullptr : &nodes[it->second];
    }
    const NodeDoc* N(const std::string& uid) const
    {
        auto it = idx.find(uid);
        return it == idx.end() ? nullptr : &nodes[it->second];
    }
    GroupDoc* G(const std::string& uid)
    {
        for (GroupDoc& g : groups)
            if (g.uid == uid) return &g;
        return nullptr;
    }
    const GroupDoc* G(const std::string& uid) const
    {
        for (const GroupDoc& g : groups)
            if (g.uid == uid) return &g;
        return nullptr;
    }
    std::string RootUid() const { return has_lib ? TokOr(lib, "root") : std::string(); }
};

const Line* ValueLine(const GroupDoc& g, const std::string& v)
{
    for (const Line& l : g.values)
        if (TokOr(l, "uid") == v) return &l;
    return nullptr;
}

bool ParseDoc(const std::string& text, Doc* d)
{
    Doc                      out;
    std::vector<std::string> lines;
    size_t                   pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string l = text.substr(pos, nl - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (lines.empty() && l.size() >= 3 && static_cast<unsigned char>(l[0]) == 0xEF) l = l.substr(3);
        if (l.find_first_not_of(" \t") != std::string::npos) lines.push_back(l);
        pos = nl + 1;
    }
    if (lines.empty() || lines[0].compare(0, 7, "RAVLIB ") != 0) return false;
    out.header = lines[0];
    enum class Cur { Pre, Group, Lib, Node } cur = Cur::Pre;
    for (size_t i = 1; i < lines.size(); ++i) {
        Line l = ParseLine(lines[i]);
        if (l.kind == "group") {
            GroupDoc g;
            g.uid = TokOr(l, "uid");
            g.group = l;
            out.groups.push_back(g);
            cur = Cur::Group;
        } else if (l.kind == "value" && cur == Cur::Group) {
            out.groups.back().values.push_back(l);
        } else if (l.kind == "library" && !out.has_lib) {
            out.has_lib = true;
            out.lib = l;
            cur = Cur::Lib;
        } else if (l.kind == "kit" && out.has_lib && cur == Cur::Lib && !out.has_kit) {
            out.has_kit = true;
            out.kit = l;
        } else if (l.kind == "node") {
            NodeDoc n;
            n.uid = TokOr(l, "uid");
            n.parent = TokOr(l, "parent");
            n.node = l;
            out.nodes.push_back(n);
            cur = Cur::Node;
        } else if (cur == Cur::Node) {
            NodeDoc& n = out.nodes.back();
            if (l.kind == "case") n.cases.push_back(l);
            else if (l.kind == "unassigned") n.unassigned.push_back(l);
            else if (l.kind == "sound" && !n.has_sound) {
                n.has_sound = true;
                n.sound = l;
            } else if (l.kind == "part") n.parts.push_back(l);
            else if (l.kind == "frame") n.frames.push_back(Var{l, {}});
            else if (l.kind == "fzone" && !n.frames.empty()) n.frames.back().fz.push_back(l);
            else if (l.kind == "zone") n.zones.push_back(l);
            else if (l.kind == "park" || l.kind == "pframe" || l.kind == "pzone") n.parked.push_back(l);
            else n.extra.push_back(l);
        } else if (cur == Cur::Group) {
            out.groups.back().extra.push_back(l);
        } else if (cur == Cur::Lib) {
            out.lib_extra.push_back(l);
        } else {
            out.pre.push_back(l);
        }
    }
    if (!out.has_lib) return false;
    out.Reindex();
    *d = std::move(out);
    return true;
}

std::string WriteDoc(const Doc& d)
{
    std::string out = d.header + "\n";
    auto        put = [&](const Line& l) { out += Str(l) + "\n"; };
    for (const Line& l : d.pre) put(l);
    for (const GroupDoc& g : d.groups) {
        put(g.group);
        for (const Line& v : g.values) put(v);
        for (const Line& e : g.extra) put(e);
    }
    put(d.lib);
    if (d.has_kit) put(d.kit);
    for (const Line& e : d.lib_extra) put(e);
    for (const NodeDoc& n : d.nodes) {
        put(n.node);
        for (const Line& c : n.cases) put(c);
        for (const Line& u : n.unassigned) put(u);
        if (n.has_sound) {
            put(n.sound);
            for (const Line& p : n.parts) put(p);
            for (const Var& v : n.frames) {
                put(v.frame);
                for (const Line& z : v.fz) put(z);
            }
            for (const Line& z : n.zones) put(z);
            for (const Line& p : n.parked) put(p);
        }
        for (const Line& e : n.extra) put(e);
    }
    return out;
}

std::vector<std::string> ChildrenIn(const Doc& d, const std::string& parent)
{
    std::vector<std::string> v;
    for (const NodeDoc& n : d.nodes)
        if (n.parent == parent && !n.parent.empty()) v.push_back(n.uid);
    return v;
}

// ---- keys ----------------------------------------------------------------------------------------------------

std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> r;
    size_t                   pos = 0;
    while (true) {
        size_t e = s.find(sep, pos);
        if (e == std::string::npos) {
            r.push_back(s.substr(pos));
            break;
        }
        r.push_back(s.substr(pos, e - pos));
        pos = e + 1;
    }
    return r;
}

std::string Join(const std::vector<std::string>& v, const std::string& sep)
{
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? sep : std::string()) + v[i];
    return s;
}

std::string KeyOf(std::initializer_list<std::string> p)
{
    return Join(std::vector<std::string>(p), "|");
}

using KV = std::map<std::string, std::string>;

bool InList(const char* const* list, size_t n, const std::string& k)
{
    for (size_t i = 0; i < n; ++i)
        if (k == list[i]) return true;
    return false;
}

const char* const kPartKeys[] = {"div", "divv", "rmap", "lanes"};
bool IsPartKey(const std::string& k) { return InList(kPartKeys, 4, k); }
bool IsFadeKey(const std::string& k) { return k == "fin" || k == "fout"; }

std::string Blob(const std::vector<Line>& v)
{
    std::vector<std::string> s;
    for (const Line& l : v) s.push_back(Canon(l));
    return Join(s, "\n");
}

std::string VarValue(const Var& v)
{
    std::string s = Canon(v.frame, {"start"});
    for (const Line& z : v.fz) s += "\n" + Canon(z);
    return s;
}

double FrameStart(const Var& v)
{
    return ToD(TokOr(v.frame, "start", "0"));
}

void Collect(const Doc& d, KV* kv)
{
    for (const NodeDoc& n : d.nodes) {
        const std::string& u = n.uid;
        (*kv)[KeyOf({"X", u})] = "1";
        for (const auto& f : FieldsOf(n.node))
            if (f.first != "uid") (*kv)[KeyOf({"F", u, "node", f.first})] = f.second;
        if (n.has_sound) {
            std::string fin, fout;
            bool        has_fin = false, has_fout = false;
            std::vector<std::string> pb;
            for (const auto& f : FieldsOf(n.sound)) {
                if (f.first == "fin") fin = f.second, has_fin = true;
                else if (f.first == "fout") fout = f.second, has_fout = true;
                else if (IsPartKey(f.first)) pb.push_back(f.first + "=" + f.second);
                else (*kv)[KeyOf({"F", u, "sound", f.first})] = f.second;
            }
            if (has_fin || has_fout) (*kv)[KeyOf({"F", u, "sound", "fades"})] = fin + "/" + fout;
            std::sort(pb.begin(), pb.end());
            std::string blob = Join(pb, " ");
            if (!n.parts.empty()) blob += "\n" + Blob(n.parts);
            if (!n.zones.empty()) blob += "\n" + Blob(n.zones);
            if (!pb.empty() || !n.parts.empty() || !n.zones.empty()) (*kv)[KeyOf({"P", u})] = blob;
            if (!n.parked.empty()) (*kv)[KeyOf({"PK", u})] = Blob(n.parked);
        }
        if (!n.extra.empty()) (*kv)[KeyOf({"XL", u})] = Blob(n.extra);
        for (const Line& c : n.cases) (*kv)[KeyOf({"C", u, TokOr(c, "value")})] = Canon(c, {"value"});
        if (!n.unassigned.empty()) {
            std::vector<std::string> ch;
            for (const Line& l : n.unassigned) ch.push_back(Canon(l));
            std::sort(ch.begin(), ch.end());
            (*kv)[KeyOf({"U", u})] = Join(ch, "\n");
        }
    }
    for (const GroupDoc& g : d.groups) {
        (*kv)[KeyOf({"GX", g.uid})] = "1";
        for (const auto& f : FieldsOf(g.group))
            if (f.first != "uid") (*kv)[KeyOf({"GF", g.uid, f.first})] = f.second;
        if (!g.extra.empty()) (*kv)[KeyOf({"GE", g.uid})] = Blob(g.extra);
        for (const Line& v : g.values) {
            const std::string vu = TokOr(v, "uid");
            (*kv)[KeyOf({"VX", g.uid, vu})] = "1";
            for (const auto& f : FieldsOf(v))
                if (f.first != "uid") (*kv)[KeyOf({"VF", g.uid, vu, f.first})] = f.second;
        }
    }
    for (const auto& f : FieldsOf(d.lib))
        if (f.first != "uid" && f.first != "root" && f.first != "ravlib" && f.first != "user") (*kv)[KeyOf({"LF", f.first})] = f.second;
    if (d.has_kit) (*kv)["LK"] = Canon(d.kit);
    if (!d.lib_extra.empty()) (*kv)["LE"] = Blob(d.lib_extra);
}

struct Opt {
    bool        has = false;
    std::string v;
    bool operator==(const Opt& o) const { return has == o.has && (!has || v == o.v); }
    bool operator!=(const Opt& o) const { return !(*this == o); }
};

// The four texts of a key: the base (merged project copy at the last sync), mine, the file, the file base (the file as
// this project last read / wrote it).
struct Trip {
    Opt b, m, f, fb;
};

enum Role { kB = 0, kM = 1, kF = 2, kFB = 3 };

struct Docs {
    const Doc* d[4] = {nullptr, nullptr, nullptr, nullptr};  // by Role; b / fb null = unknown
    const Doc* at(int i) const { return d[i]; }
};

Opt& Slot(Trip& t, int role)
{
    return role == kB ? t.b : role == kM ? t.m : role == kF ? t.f : t.fb;
}

// Variation clusters of one node (review 13-77b L1): the frames of the texts matched by order and nearest start, at
// most one frame per text in a cluster, each cluster anchored on its first start (no chaining); two frames of one text
// less than 1 ms apart are two clusters. The id is the anchor in ms, "~2", "~3" ... when it repeats.
struct Cluster {
    std::string id;
    double      anchor = 0.0;
    int         member[4] = {-1, -1, -1, -1};  // frame index per Role
};

std::vector<Cluster> ClustersOf(const Docs& ds, const std::string& uid)
{
    struct Item {
        double start;
        int    doc, idx;
    };
    std::vector<Item> items;
    for (int di = 0; di < 4; ++di) {
        const Doc* d = ds.at(di);
        if (!d) continue;
        const NodeDoc* n = d->N(uid);
        if (!n) continue;
        for (size_t i = 0; i < n->frames.size(); ++i) items.push_back({FrameStart(n->frames[i]), di, static_cast<int>(i)});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.start != b.start ? a.start < b.start : a.doc < b.doc; });
    std::vector<Cluster> cl;
    const double         tol = 0.00105;
    for (const Item& it : items) {
        int    best = -1;
        double bd = 1e18;
        for (int k = static_cast<int>(cl.size()) - 1; k >= 0; --k) {
            const double dd = it.start - cl[static_cast<size_t>(k)].anchor;
            if (dd > tol) break;
            if (cl[static_cast<size_t>(k)].member[it.doc] >= 0) continue;
            if (std::fabs(dd) < bd) {
                bd = std::fabs(dd);
                best = k;
            }
        }
        if (best < 0) {
            Cluster c;
            c.anchor = it.start;
            cl.push_back(c);
            best = static_cast<int>(cl.size()) - 1;
        }
        cl[static_cast<size_t>(best)].member[it.doc] = it.idx;
    }
    std::map<long long, int> seen;
    for (Cluster& c : cl) {
        const long long ms = std::llround(c.anchor * 1000.0);
        const int       k = ++seen[ms];
        c.id = std::to_string(ms) + (k > 1 ? "~" + std::to_string(k) : std::string());
    }
    return cl;
}

// ---- display -------------------------------------------------------------------------------------------------

std::string NameIn(const Doc* d, const std::string& uid)
{
    if (!d) return std::string();
    const NodeDoc* n = d->N(uid);
    if (!n) return std::string();
    return PctDecode(TokOr(n->node, "name"));
}

std::string TypeWord(const std::string& t)
{
    if (t == "sound") return "Sound";
    if (t == "random") return "Random";
    if (t == "blend") return "Blend";
    if (t == "switch") return "Switch";
    return "Node";
}

const int kNameOrder[4] = {kM, kF, kB, kFB};

struct Describe {
    const Docs& ds;

    std::string NodeName(const std::string& uid) const
    {
        for (int i : kNameOrder) {
            const std::string n = NameIn(ds.at(i), uid);
            if (!n.empty()) return n;
        }
        return uid;
    }
    const NodeDoc* AnyNode(const std::string& uid) const
    {
        for (int i : kNameOrder)
            if (ds.at(i) && ds.at(i)->N(uid)) return ds.at(i)->N(uid);
        return nullptr;
    }
    std::string ValueName(const std::string& vuid) const
    {
        for (int i : kNameOrder) {
            const Doc* d = ds.at(i);
            if (!d) continue;
            for (const GroupDoc& g : d->groups)
                if (const Line* l = ValueLine(g, vuid)) return PctDecode(TokOr(*l, "name", vuid));
        }
        return vuid;
    }
    std::string GroupName(const std::string& guid) const
    {
        for (int i : kNameOrder) {
            const Doc* d = ds.at(i);
            if (d && d->G(guid)) return PctDecode(TokOr(d->G(guid)->group, "name", guid));
        }
        return guid;
    }
    std::string Type(const std::string& uid) const
    {
        const NodeDoc* n = AnyNode(uid);
        return n ? TokOr(n->node, "type", "random") : std::string("random");
    }
    int SoundsUnder(const Doc* d, const std::string& uid) const
    {
        if (!d) return 0;
        int n = 0;
        for (const std::string& c : ChildrenIn(*d, uid)) {
            const NodeDoc* cn = d->N(c);
            if (cn && TokOr(cn->node, "type") == "sound") ++n;
            else n += SoundsUnder(d, c);
        }
        return n;
    }
};

struct Shown {
    std::string label, from, to, unit;
    bool        rename = false;
};

Shown ShowField(const Describe& ds, const std::string& sect, const std::string& key, const Opt& a, const Opt& b)
{
    Shown       s;
    auto        val = [&](const Opt& o, const std::string& def) { return o.has ? PctDecode(o.v) : def; };
    std::string def;
    if (key == "name") {
        s.label = "Name";
        s.rename = true;
    } else if (key == "vol") {
        s.label = "Gain";
        s.unit = " dB";
        def = "0";
    } else if (key == "offset_ms") {
        s.label = "Offset";
        s.unit = " ms";
        def = "0";
    } else if (key == "avoid") {
        s.label = "Avoid repeat";
        def = "2";
    } else if (key == "pitch") {
        s.label = "Pitch";
    } else if (key == "lands") {
        s.label = "Lands in";
        def = "inherited";
    } else if (key == "default") {
        s.label = "Default value";
        def = "the group's";
    } else if (key == "preview") {
        s.label = "Preview value";
        def = "the default";
    } else if (key == "type") {
        s.label = "Type";
    } else if (key == "fades") {
        s.label = "Fades";
        s.unit = " ms";
    } else {
        s.label = key + " changed";
    }
    auto render = [&](const Opt& o) {
        if (key == "default" || key == "preview") return o.has ? ds.ValueName(o.v) : def;
        if (key == "fades") {
            if (!o.has) return std::string("default");
            const size_t sl = o.v.find('/');
            const std::string fi = o.v.substr(0, sl), fo = sl == std::string::npos ? std::string() : o.v.substr(sl + 1);
            return (fi.empty() ? std::string("-") : Num(ToD(fi) * 1000.0)) + "/" + (fo.empty() ? std::string("-") : Num(ToD(fo) * 1000.0));
        }
        if (key == "vol" || key == "offset_ms") return Num(ToD(val(o, def)));
        return val(o, def);
    };
    (void)sect;
    s.from = render(a);
    s.to = render(b);
    return s;
}


// ---- classification --------------------------------------------------------------------------------------------

enum class Cls { None, Here, File, Both };

// Review 13-77b H3: here = mine moved from the base, file = the file moved from the file base. Neither moved while they
// differ = an adaptation of the merge (never listed). `known` false (no common version, or the key's owner is missing
// from a common version): every difference is Both.
Cls Classify(const Trip& t, bool known)
{
    if (t.m == t.f) return Cls::None;
    if (!known) return Cls::Both;
    const bool here = t.m != t.b, file = t.f != t.fb;
    if (here && file) return Cls::Both;
    if (here) return Cls::Here;
    if (file) return Cls::File;
    return Cls::None;
}

SyncSide SideOf(Cls c)
{
    return c == Cls::Here ? SyncSide::Here : c == Cls::File ? SyncSide::File : SyncSide::Both;
}

bool NodeScoped(const std::string& kind)
{
    return kind == "F" || kind == "P" || kind == "PK" || kind == "XL" || kind == "U" || kind == "O" || kind == "V" || kind == "C";
}

struct Pres {
    bool b = false, m = false, f = false, fb = false;
};

Opt MakeOpt(const std::string* v)
{
    Opt o;
    if (v) o.has = true, o.v = *v;
    return o;
}

}  // namespace

void SyncDiff::BuildIndex()
{
    index.clear();
    needed_by.clear();
    index.reserve(changes.size());
    for (size_t i = 0; i < changes.size(); ++i) index[changes[i].key] = i;
    for (size_t i = 0; i < changes.size(); ++i)
        for (const std::string& n : changes[i].needs) needed_by[n].push_back(i);
}

const SyncChange* SyncDiff::Find(const std::string& key) const
{
    if (IndexValid()) {
        const auto it = index.find(key);
        if (it == index.end()) return nullptr;
        if (it->second < changes.size() && changes[it->second].key == key) return &changes[it->second];
    }
    for (const SyncChange& c : changes)
        if (c.key == key) return &c;
    return nullptr;
}

namespace {

int DepthOf(const std::map<std::string, std::string>& parent, std::string uid)
{
    int n = 0;
    while (!uid.empty() && n < 1000) {
        auto it = parent.find(uid);
        if (it == parent.end() || it->second.empty()) break;
        uid = it->second;
        ++n;
    }
    return n;
}

std::set<std::string> SubtreeIn(const Doc& d, const std::string& top)
{
    std::set<std::string>                   sub;
    std::function<void(const std::string&)> walk = [&](const std::string& u) {
        if (!sub.insert(u).second) return;
        for (const std::string& c : ChildrenIn(d, u)) walk(c);
    };
    walk(top);
    return sub;
}

}  // namespace

SyncDiff DiffLibrary(const std::string& base_text, const std::string& mine_text, const std::string& file_text)
{
    return DiffLibrary(base_text, base_text, mine_text, file_text);
}

SyncDiff DiffLibrary(const std::string& base_text, const std::string& file_base_text, const std::string& mine_text,
                     const std::string& file_text)
{
    SyncDiff out;
    Doc      db, dfb, dm, df;
    if (!ParseDoc(mine_text, &dm) || !ParseDoc(file_text, &df)) return out;
    const bool known = !base_text.empty() && !file_base_text.empty() && ParseDoc(base_text, &db) && ParseDoc(file_base_text, &dfb);
    out.had_base = known;
    out.root = dm.RootUid();
    Docs ds;
    ds.d[kB] = known ? &db : nullptr;
    ds.d[kM] = &dm;
    ds.d[kF] = &df;
    ds.d[kFB] = known ? &dfb : nullptr;
    Describe desc{ds};

    // union tree (mine's parent first)
    for (int i : kNameOrder) {
        const Doc* d = ds.at(i);
        if (!d) continue;
        for (const NodeDoc& n : d->nodes)
            if (!out.parent.count(n.uid)) out.parent[n.uid] = n.parent;
    }
    for (const NodeDoc& n : dm.nodes) out.mine_nodes.insert(n.uid);

    // the keys of the four texts
    std::map<std::string, Trip> trips;
    for (int i = 0; i < 4; ++i) {
        const Doc* d = ds.at(i);
        if (!d) continue;
        KV kv;
        Collect(*d, &kv);
        for (const auto& e : kv) Slot(trips[e.first], i) = MakeOpt(&e.second);
    }
    std::map<std::string, Pres> pres;
    for (int i = 0; i < 4; ++i) {
        const Doc* d = ds.at(i);
        if (!d) continue;
        for (const NodeDoc& n : d->nodes) {
            Pres& p = pres[n.uid];
            (i == kB ? p.b : i == kM ? p.m : i == kF ? p.f : p.fb) = true;
        }
    }
    // variations and order
    std::map<std::string, std::vector<Cluster>> var_clusters;
    for (const auto& pp : pres) {
        const std::string& u = pp.first;
        if (!pp.second.m && !pp.second.f) continue;
        const std::vector<Cluster> cl = ClustersOf(ds, u);
        var_clusters[u] = cl;
        for (const Cluster& c : cl) {
            Trip& t = trips[KeyOf({"V", u, c.id})];
            for (int di = 0; di < 4; ++di) {
                const Doc* d = ds.at(di);
                if (!d || c.member[di] < 0) continue;
                Slot(t, di) = Opt{true, VarValue(d->N(u)->frames[static_cast<size_t>(c.member[di])])};
            }
        }
        // order of the children present everywhere
        std::vector<std::string> kids[4];
        for (int di = 0; di < 4; ++di) {
            const Doc* d = ds.at(di);
            if (!d) continue;
            for (const std::string& c : ChildrenIn(*d, u)) {
                const Pres& cp = pres[c];
                if (cp.m && cp.f && (!known || (cp.b && cp.fb))) kids[di].push_back(c);
            }
        }
        // v5: the order of a Random container means nothing (it picks at random): never a change; a Blend keeps its order
        if (kids[kM].size() >= 2 && pp.second.m && pp.second.f && desc.Type(u) != "random") {
            Trip& t = trips[KeyOf({"O", u})];
            for (int di = 0; di < 4; ++di)
                if (ds.at(di) && ds.at(di)->N(u)) Slot(t, di) = Opt{true, Join(kids[di], ",")};
        }
    }

    auto key_node = [](const std::string& key) {
        const std::vector<std::string> p = Split(key, '|');
        return p.size() > 1 ? p[1] : std::string();
    };
    // Is the key's owner (node / group / value) in both common versions? Not: no common version for it (both sides
    // added it on their own): every difference reads as both.
    auto owner_known = [&](const std::vector<std::string>& p) {
        if (!known) return false;
        const std::string& kind = p[0];
        if (NodeScoped(kind)) {
            const Pres& pr = pres[p[1]];
            return pr.b && pr.fb;
        }
        if (kind == "GF" || kind == "GE") return db.G(p[1]) && dfb.G(p[1]);
        if (kind == "VF") return db.G(p[1]) && dfb.G(p[1]) && ValueLine(*db.G(p[1]), p[2]) && ValueLine(*dfb.G(p[1]), p[2]);
        return true;
    };

    // count of changed keys of a node subtree on one side (decision 4): mine against the base, the file against the file
    // base; added nodes count.
    auto subtree_changes = [&](const std::string& top, const Doc& holder, bool file_side) {
        const std::set<std::string> sub = SubtreeIn(holder, top);
        int                         n = 0;
        for (const auto& kv : trips) {
            const std::vector<std::string> p = Split(kv.first, '|');
            if (p.size() < 2 || !(NodeScoped(p[0]) || p[0] == "X") || !sub.count(p[1])) continue;
            if (p[0] == "X" && p[1] == top) continue;
            if (p[0] != "X" && !(file_side ? pres[p[1]].fb : pres[p[1]].b)) continue;  // an added node counts once
            if (file_side ? kv.second.f != kv.second.fb : kv.second.m != kv.second.b) ++n;
        }
        return n;
    };

    std::vector<std::string> both_added;  // nodes both sides added (the base gets them once they agree, review M2)
    std::vector<std::string> both_added_f;  // the same seen from the file base (second review H1)
    auto add = [&](SyncChange c, Cls cl) {
        c.side = SideOf(cl);
        out.changes.push_back(std::move(c));
    };

    for (const auto& kv : trips) {
        const std::string& key = kv.first;
        const Trip&        t = kv.second;
        const std::vector<std::string> p = Split(key, '|');
        const std::string& kind = p[0];
        if (kind == "X") {
            const std::string& u = p[1];
            const Pres&        pr = pres[u];
            if (pr.m == pr.f) {
                if (known && pr.m && !pr.b) both_added.push_back(u);
                else if (known && !pr.m && pr.b) out.folds.push_back(key);  // removed on both sides: the base drops it
                if (known && pr.f && !pr.fb) both_added_f.push_back(u);  // the file base's twins (second review H1)
                else if (known && !pr.f && pr.fb) out.file_folds.push_back(key);
                continue;
            }
            SyncChange c;
            c.key = key;
            c.node = u;
            c.subject = u;
            c.depth = DepthOf(out.parent, u);
            const std::string type = desc.Type(u);
            c.adds_container = type != "sound";
            const std::string nm = desc.NodeName(u);
            const bool        sound = type == "sound";
            c.label = nm;
            if (sound) c.node = out.parent[u];
            const bool here = pr.m != pr.b, file = pr.f != pr.fb;
            if (!known || (here && file)) {
                c.here_text = pr.m ? "present" : "absent";
                c.file_text = pr.f ? "present" : "absent";
                c.text = TypeWord(type) + " " + nm + ": here " + c.here_text + " · global " + c.file_text;
                add(c, Cls::Both);
                continue;
            }
            if (!here && !file) continue;  // an adaptation of the merge
            Cls cl;
            if (here ? !pr.m : !pr.f) {  // removed by the side that moved
                const bool     mine_removed = here;
                const Doc*     lack = mine_removed ? &dm : &df;
                const Doc*     keep = mine_removed ? &df : &dm;
                const NodeDoc* kn = keep->N(u);
                const std::string par = kn ? kn->parent : out.parent[u];
                if (par.empty() || !lack->N(par)) continue;  // not the topmost removed
                const int n = subtree_changes(u, *keep, mine_removed);
                const int sounds = desc.SoundsUnder(keep, u);
                const std::string tail = !sound && sounds > 0 ? " (" + std::to_string(sounds) + (sounds == 1 ? " sound)" : " sounds)") : std::string();
                if (n > 0) {
                    cl = Cls::Both;
                    c.here_text = mine_removed ? "removed" : std::to_string(n) + (n == 1 ? " change" : " changes");
                    c.file_text = mine_removed ? std::to_string(n) + (n == 1 ? " change" : " changes") : "removed";
                    c.text = TypeWord(type) + " " + nm + ": here " + c.here_text + " · global " + c.file_text;
                } else {
                    cl = mine_removed ? Cls::Here : Cls::File;
                    c.here_text = mine_removed ? "removed" : "present";
                    c.file_text = mine_removed ? "present" : "removed";
                    c.text = TypeWord(type) + " " + nm + " removed" + tail;
                }
            } else {  // added by the side that moved
                cl = here ? Cls::Here : Cls::File;
                c.here_text = pr.m ? "present" : "absent";
                c.file_text = pr.f ? "present" : "absent";
                c.text = TypeWord(type) + " " + nm + " added";
            }
            add(c, cl);
            continue;
        }
        if (kind == "GX" || kind == "VX") {
            if (!known) continue;  // no common version: the Apply copies what a picked change references
            if (t.m == t.f) {
                if (t.m != t.b) out.folds.push_back(key);
                if (t.f != t.fb) out.file_folds.push_back(key);
                continue;
            }
            // decision 5: only an addition is a change (a group / value missing on one side is never removed through sync)
            const bool here_add = t.m.has && !t.b.has && !t.f.has;
            const bool file_add = t.f.has && !t.fb.has && !t.m.has;
            if (!here_add && !file_add) continue;
            SyncChange c;
            c.key = key;
            c.node = "grp:" + p[1];
            c.subject = c.node;
            const Cls cl = here_add ? Cls::Here : Cls::File;
            if (kind == "GX") {
                c.text = "Switch Group " + desc.GroupName(p[1]) + " added";
                c.label = desc.GroupName(p[1]);
            } else {
                c.text = "Value " + desc.ValueName(p[2]) + " added";
                c.label = desc.ValueName(p[2]);
            }
            c.here_text = t.m.has ? "present" : "absent";
            c.file_text = t.f.has ? "present" : "absent";
            add(c, cl);
            continue;
        }
        if (kind == "GF" || kind == "GE" || kind == "VF") {
            // the group / value must be on both sides
            const std::string g = p[1];
            if (!dm.G(g) || !df.G(g)) continue;
            if (kind == "VF" && (!ValueLine(*dm.G(g), p[2]) || !ValueLine(*df.G(g), p[2]))) continue;
        }
        if (NodeScoped(kind)) {
            const std::string& u = p[1];
            const Pres&        pr = pres[u];
            if (kind == "C") {
                if (!pr.m && !pr.f) continue;
                if ((!pr.m || !pr.f) && known && (pr.b || pr.fb)) continue;  // under a removed node
            } else if (!(pr.m && pr.f)) {
                continue;
            }
        }
        const Cls cl = Classify(t, owner_known(p));
        if (cl == Cls::None) {
            if (known && t.m == t.f && t.m != t.b) out.folds.push_back(key);
            if (known && t.m == t.f && t.f != t.fb) out.file_folds.push_back(key);
            continue;
        }
        SyncChange c;
        c.key = key;
        c.depth = 0;
        c.node = NodeScoped(kind) ? key_node(key) : (kind[0] == 'L' ? out.root : "grp:" + key_node(key));
        c.subject = c.node;
        if (NodeScoped(kind)) {
            const NodeDoc* nd = desc.AnyNode(c.node);
            c.depth = DepthOf(out.parent, c.node);
            if (nd && TokOr(nd->node, "type") == "sound" && kind != "C") {
                const std::string par = out.parent[c.node];
                if (!par.empty()) c.node = par;
            }
        }
        const std::string subject = NodeScoped(kind) ? desc.NodeName(key_node(key)) : std::string();
        const bool        sound_subject = NodeScoped(kind) && desc.Type(key_node(key)) == "sound";
        auto of = [&](const std::string& lab) { return sound_subject ? lab + " of " + subject : lab; };
        auto sentence = [&](const std::string& lab, const std::string& from, const std::string& to) {
            return of(lab) + " " + from + " → " + to;
        };
        if (kind == "F" && p[2] == "node" && p[3] == "parent") {
            // review 13-77b H1: a move, listed under its new parent (that side's)
            auto pname = [&](const Opt& o) { return o.has && !o.v.empty() ? desc.NodeName(o.v) : std::string("the root"); };
            c.label = "Moved";
            c.here_text = "in " + pname(t.m);
            c.file_text = "in " + pname(t.f);
            if (cl == Cls::Both) {
                c.text = "Moved " + subject + ": here to " + pname(t.m) + " · global to " + pname(t.f);
            } else {
                const Opt& from = cl == Cls::Here ? t.b : t.fb;
                const Opt& to = cl == Cls::Here ? t.m : t.f;
                c.text = "Moved " + subject + " from " + pname(from) + " to " + pname(to);
            }
            const Opt& np = cl == Cls::File ? t.f : t.m;
            if (np.has && !np.v.empty()) c.node = np.v;
        } else if (kind == "F") {
            const Shown sh = ShowField(desc, p[2], p[3], t.b, t.m);  // labels / units
            const Shown mh = ShowField(desc, p[2], p[3], t.b, t.m);
            const Shown fh = ShowField(desc, p[2], p[3], t.fb, t.f);
            c.label = sh.label;
            c.here_text = mh.to + mh.unit;
            c.file_text = fh.to + fh.unit;
            if (mh.rename) {
                c.text = cl == Cls::Both ? "Renamed: here " + mh.to + " · global " + fh.to : "Renamed " + (cl == Cls::Here ? mh.from : fh.from) + " → " + (cl == Cls::Here ? mh.to : fh.to);
            } else if (sh.label.size() > 8 && sh.label.compare(sh.label.size() - 8, 8, " changed") == 0) {
                c.text = of(sh.label);
                if (sound_subject) c.text = sh.label + " (" + subject + ")";
            } else if (cl == Cls::Both) {
                c.text = of(sh.label) + ": here " + c.here_text + " · global " + c.file_text;
            } else {
                const Shown& s2 = cl == Cls::Here ? mh : fh;
                c.text = sentence(sh.label, s2.from, s2.to) + s2.unit;
            }
        } else if (kind == "P") {
            c.label = "Parts";
            c.text = "Parts of " + subject + " changed";
            c.here_text = "here";
            c.file_text = "file";
        } else if (kind == "PK") {
            c.label = "Parked variations";
            c.text = "Kept variations of " + subject + " changed";
        } else if (kind == "XL") {
            c.label = "Other settings";
            c.text = "Other settings of " + subject + " changed";
        } else if (kind == "U") {
            c.label = "Unassigned";
            c.text = "Unassigned children of " + subject + " changed";
        } else if (kind == "O") {
            c.label = "Order";
            c.text = "Order of children changed";
            c.here_text = "here order";
            c.file_text = "global order";
        } else if (kind == "V") {
            const std::vector<Cluster>& cl2 = var_clusters[p[1]];
            int                         ix = 0, k = 0;
            for (const Cluster& cc : cl2) {
                ++k;
                if (cc.id == p[2]) ix = k;
            }
            c.label = "Variation " + std::to_string(ix);
            const std::string vn = "Variation " + std::to_string(ix) + " of " + subject;
            c.here_text = t.m.has ? "present" : "absent";
            c.file_text = t.f.has ? "present" : "absent";
            const Opt& side = cl == Cls::File ? t.f : t.m;
            const Opt& from = cl == Cls::File ? t.fb : t.b;
            if (cl == Cls::Both) c.text = vn + ": here " + (t.m.has ? "changed" : "removed") + " · global " + (t.f.has ? "changed" : "removed");
            else if (!side.has) c.text = vn + " removed";
            else if (!from.has) c.text = vn + " added";
            else c.text = vn + " changed";
        } else if (kind == "C") {
            const std::string vname = desc.ValueName(p[2]);
            auto childname = [&](const Opt& o) {
                if (!o.has) return std::string();
                const Line l = ParseLine("case " + o.v);
                return desc.NodeName(TokOr(l, "child"));
            };
            c.label = "Switch value " + vname;
            c.here_text = t.m.has ? "plays " + childname(t.m) : "absent";
            c.file_text = t.f.has ? "plays " + childname(t.f) : "absent";
            const Opt& side = cl == Cls::File ? t.f : t.m;
            const Opt& from = cl == Cls::File ? t.fb : t.b;
            if (cl == Cls::Both) c.text = "Switch value " + vname + ": here " + c.here_text + " · global " + c.file_text;
            else if (!side.has) c.text = "Switch value " + vname + " removed";
            else {
                // 13-77 feedback: it is not the value that is new. A child this side added travels with its case.
                const std::string kid = TokOr(ParseLine("case " + side.v), "child");
                const auto        kp = pres.find(kid);
                const bool        kid_new = kp != pres.end() && (cl == Cls::File ? !kp->second.fb : !kp->second.b);
                c.text = vname + " now plays " + childname(side) + (kid_new ? " (new)" : "");
            }
        } else if (kind == "GF") {
            const Shown mh = ShowField(desc, "group", p[2], t.b, t.m), fh = ShowField(desc, "group", p[2], t.fb, t.f);
            c.label = mh.label;
            c.here_text = mh.to;
            c.file_text = fh.to;
            c.text = "Switch Group " + desc.GroupName(p[1]) + ": " + mh.label + (cl == Cls::Both ? ": here " + mh.to + " · global " + fh.to : cl == Cls::Here ? " " + mh.from + " → " + mh.to : " " + fh.from + " → " + fh.to);
            if (p[2] == "name" && cl != Cls::Here) c.text += " (shared by every library of this project that uses it)";  // review L6
        } else if (kind == "GE") {
            c.label = "Other settings";
            c.text = "Other settings of Switch Group " + desc.GroupName(p[1]) + " changed";
        } else if (kind == "VF") {
            const Shown mh = ShowField(desc, "value", p[3], t.b, t.m), fh = ShowField(desc, "value", p[3], t.fb, t.f);
            c.label = p[3] == "roles" ? "Roles" : mh.label;
            c.here_text = mh.to;
            c.file_text = fh.to;
            c.text = "Value " + desc.ValueName(p[2]) + ": " + c.label + (cl == Cls::Both ? ": here " + mh.to + " · global " + fh.to : cl == Cls::Here ? " " + mh.from + " → " + mh.to : " " + fh.from + " → " + fh.to);
        } else if (kind == "LF") {
            const Shown mh = ShowField(desc, "lib", p[1], t.b, t.m), fh = ShowField(desc, "lib", p[1], t.fb, t.f);
            c.label = p[1] == "name" ? "Library name" : p[1] == "color" ? "Colour" : mh.label;
            c.here_text = mh.to;
            c.file_text = fh.to;
            if (p[1] == "name")
                c.text = cl == Cls::Both ? "Renamed library: here " + mh.to + " · global " + fh.to : "Renamed library " + (cl == Cls::Here ? mh.from : fh.from) + " → " + (cl == Cls::Here ? mh.to : fh.to);
            else c.text = "Library " + c.label + " changed";
        } else if (kind == "LK") {
            c.label = "Kit";
            c.text = "Library kit changed";
        } else {
            c.label = "Library";
            c.text = "Other library settings changed";
        }
        add(c, cl);
    }

    // Review 13-77b M1 (decision 4): a node removed on one side and changed on the other is ONE both change: the changes
    // inside it (an edit, a Sound added under it) are not listed on their own.
    {
        std::set<std::string> swallowed;
        for (const SyncChange& c : out.changes) {
            if (c.side != SyncSide::Both || c.key.compare(0, 2, "X|") != 0) continue;
            if (c.here_text != "removed" && c.file_text != "removed") continue;
            const Doc&                  keep = c.here_text == "removed" ? df : dm;
            const std::set<std::string> sub = SubtreeIn(keep, c.subject);
            for (const SyncChange& o : out.changes)
                if (o.key != c.key && o.subject.compare(0, 4, "grp:") != 0 && sub.count(o.subject)) swallowed.insert(o.key);
        }
        if (!swallowed.empty())
            out.changes.erase(std::remove_if(out.changes.begin(), out.changes.end(), [&](const SyncChange& c) { return swallowed.count(c.key) > 0; }),
                              out.changes.end());
    }

    // nodes both sides added: into the base once every key of theirs agrees (until then they read as both)
    if (!both_added.empty() || !both_added_f.empty()) {
        std::set<std::string> differs;
        for (const auto& kv : trips) {
            const std::vector<std::string> p = Split(kv.first, '|');
            if (p.size() > 1 && NodeScoped(p[0]) && kv.second.m != kv.second.f) differs.insert(p[1]);
        }
        for (const std::string& u : both_added)
            if (!differs.count(u)) out.folds.push_back(KeyOf({"X", u}));
        for (const std::string& u : both_added_f)
            if (!differs.count(u)) out.file_folds.push_back(KeyOf({"X", u}));
    }

    // needs (same side, never for Both)
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < out.changes.size(); ++i) at[out.changes[i].key] = i;
    auto need = [&](SyncChange& c, const std::string& key) {
        auto it = at.find(key);
        if (it == at.end() || out.changes[it->second].side != c.side || key == c.key) return;
        if (std::find(c.needs.begin(), c.needs.end(), key) == c.needs.end()) c.needs.push_back(key);
    };
    auto group_of_value = [&](const Doc* d, const std::string& v) {
        if (d)
            for (const GroupDoc& g : d->groups)
                if (ValueLine(g, v)) return g.uid;
        return std::string();
    };
    // child node -> the case keys that play it, once per side and version (now: m / f; common version: b / fb)
    std::map<std::string, std::vector<std::string>> playedNowHere, playedNowFile, playedWasHere, playedWasFile;
    for (const auto& kv : trips) {
        if (kv.first.compare(0, 2, "C|") != 0) continue;
        auto put = [&](const Opt& o, std::map<std::string, std::vector<std::string>>& m) {
            if (o.has) m[TokOr(ParseLine("case " + o.v), "child")].push_back(kv.first);
        };
        put(kv.second.m, playedNowHere);
        put(kv.second.f, playedNowFile);
        put(kv.second.b, playedWasHere);
        put(kv.second.fb, playedWasFile);
    }
    auto casesOf = [](const std::map<std::string, std::vector<std::string>>& m, const std::string& u) {
        const auto it = m.find(u);
        static const std::vector<std::string> none;
        return it == m.end() ? none : it->second;
    };
    for (SyncChange& c : out.changes) {
        if (c.side == SyncSide::Both) continue;
        const bool        here = c.side == SyncSide::Here;
        const Doc*        sd = here ? &dm : &df;  // the doc that holds the change
        const std::vector<std::string> p = Split(c.key, '|');
        if (p[0] == "X") {
            const std::string& u = p[1];
            if (const NodeDoc* n = sd->N(u)) {  // added: its parent on that side
                need(c, KeyOf({"X", n->parent}));
                // 13-77 feedback: the case that makes it play travels with it (the case needs the node too)
                for (const std::string& k : casesOf(here ? playedNowHere : playedNowFile, u)) need(c, k);
            } else {  // removal: the cases that played it (in that side's common version) go too
                for (const std::string& k : casesOf(here ? playedWasHere : playedWasFile, u)) need(c, k);
            }
        } else if (p[0] == "C") {
            const Trip& t = trips[c.key];
            const Opt&  side = here ? t.m : t.f;
            need(c, KeyOf({"X", p[1]}));
            if (!side.has) {  // a removed case goes with the removal of the node it played (symmetric)
                const Opt& was = here ? t.b : t.fb;
                if (was.has) need(c, KeyOf({"X", TokOr(ParseLine("case " + was.v), "child")}));
            }
            if (side.has) {
                const std::string child = TokOr(ParseLine("case " + side.v), "child");
                need(c, KeyOf({"X", child}));
                const std::string g = group_of_value(sd, p[2]);
                if (!g.empty()) need(c, KeyOf({"VX", g, p[2]}));
            }
        } else if (p[0] == "VX") {
            need(c, KeyOf({"GX", p[1]}));
        } else if (p[0] == "F" && p.size() > 3 && p[2] == "node" && p[3] == "parent") {
            need(c, KeyOf({"X", p[1]}));
            if (const NodeDoc* n = sd->N(p[1])) need(c, KeyOf({"X", n->parent}));  // review H1: its new parent
        } else if (p[0] == "U" || p[0] == "F" || p[0] == "P" || p[0] == "V") {
            need(c, KeyOf({"X", p[1]}));
        }
    }

    // rows (review H5 / M8): the project row each change belongs to, and the counts per row, once
    for (SyncChange& c : out.changes) {
        if (c.node.compare(0, 4, "grp:") == 0) {
            c.row = out.root;
        } else {
            std::string u = c.node;
            for (int i = 0; i < 1000 && !u.empty(); ++i) {
                if (out.mine_nodes.count(u)) break;
                auto it = out.parent.find(u);
                u = it == out.parent.end() ? std::string() : it->second;
            }
            c.row = out.mine_nodes.count(u) ? u : out.root;
        }
        LibSyncCounts& rc = out.row_counts[c.row];
        (c.side == SyncSide::Here ? rc.here : c.side == SyncSide::File ? rc.file : rc.both)++;
        if (c.side == SyncSide::Here) ++out.here;
        else if (c.side == SyncSide::File) ++out.file;
        else ++out.both;
    }
    bool untouched = known;
    for (const auto& kv : trips)
        if (untouched && kv.second.f != kv.second.fb) untouched = false;
    out.file_untouched = untouched;
    std::sort(out.folds.begin(), out.folds.end());
    out.folds.erase(std::unique(out.folds.begin(), out.folds.end()), out.folds.end());
    std::sort(out.file_folds.begin(), out.file_folds.end());
    out.file_folds.erase(std::unique(out.file_folds.begin(), out.file_folds.end()), out.file_folds.end());
    out.BuildIndex();
    return out;
}

std::vector<std::string> NeedsClosure(const SyncDiff& d, const std::string& key)
{
    std::set<std::string>    seen;
    std::vector<std::string> todo = {key};
    while (!todo.empty()) {
        const std::string k = todo.back();
        todo.pop_back();
        const SyncChange* c = d.Find(k);
        if (!c) continue;
        for (const std::string& n : c->needs)
            if (seen.insert(n).second) todo.push_back(n);
    }
    seen.erase(key);
    return std::vector<std::string>(seen.begin(), seen.end());
}

std::vector<std::string> DependentsClosure(const SyncDiff& d, const std::string& key)
{
    std::set<std::string>    seen;
    std::vector<std::string> todo = {key};
    while (!todo.empty()) {
        const std::string k = todo.back();
        todo.pop_back();
        if (d.IndexValid()) {
            const auto it = d.needed_by.find(k);
            if (it == d.needed_by.end()) continue;
            for (size_t i : it->second)
                if (i < d.changes.size() && seen.insert(d.changes[i].key).second) todo.push_back(d.changes[i].key);
            continue;
        }
        for (const SyncChange& c : d.changes)
            if (std::find(c.needs.begin(), c.needs.end(), k) != c.needs.end() && seen.insert(c.key).second) todo.push_back(c.key);
    }
    seen.erase(key);
    return std::vector<std::string>(seen.begin(), seen.end());
}

bool SyncIsAddition(const SyncChange& c, SyncSide* adder)
{
    if (c.side == SyncSide::Here && c.file_text == "absent" && c.here_text != "absent") {
        if (adder) *adder = SyncSide::Here;
        return true;
    }
    if (c.side == SyncSide::File && c.here_text == "absent" && c.file_text != "absent") {
        if (adder) *adder = SyncSide::File;
        return true;
    }
    return false;
}

bool SyncSideAvailable(const SyncChange& c, SyncDir dir)
{
    SyncSide adder;
    if (!SyncIsAddition(c, &adder)) return true;
    return dir == (adder == SyncSide::Here ? SyncDir::Up : SyncDir::Down);
}

std::vector<std::string> DropDependents(const SyncDiff& d, const std::string& key, SyncDir dir)
{
    // A drop never takes an addition of the side being dropped (an addition is never deleted by the other side), nor
    // what is reached only through that addition.
    std::set<std::string>    seen;
    std::vector<std::string> todo = {key};
    while (!todo.empty()) {
        const std::string k = todo.back();
        todo.pop_back();
        std::vector<std::string> direct;
        if (d.IndexValid()) {
            const auto it = d.needed_by.find(k);
            if (it != d.needed_by.end())
                for (size_t i : it->second)
                    if (i < d.changes.size()) direct.push_back(d.changes[i].key);
        } else {
            for (const SyncChange& c : d.changes)
                if (std::find(c.needs.begin(), c.needs.end(), k) != c.needs.end()) direct.push_back(c.key);
        }
        for (const std::string& n : direct) {
            if (n == key) continue;
            const SyncChange* x = d.Find(n);
            if (!x || !SyncSideAvailable(*x, dir)) continue;
            if (seen.insert(n).second) todo.push_back(n);
        }
    }
    return std::vector<std::string>(seen.begin(), seen.end());
}

std::map<std::string, SyncDir> CloseSelection(const SyncDiff& d, const std::map<std::string, SyncDir>& picks)
{
    std::map<std::string, SyncDir> out;
    for (const auto& pk : picks) {
        const SyncChange* c = d.Find(pk.first);
        if (!c) continue;
        out[pk.first] = pk.second;
        if (c->side == SyncSide::Both) continue;
        const bool adopt = (c->side == SyncSide::Here) == (pk.second == SyncDir::Up);
        for (const std::string& k : adopt ? NeedsClosure(d, pk.first) : DropDependents(d, pk.first, pk.second)) out[k] = pk.second;
    }
    return out;
}

std::vector<std::string> ContainerChangeKeys(const SyncDiff& d, const std::string& container)
{
    std::vector<std::string> keys;
    for (const SyncChange& c : d.changes)
        if (c.row == container) keys.push_back(c.key);
    return keys;
}

LibSyncCounts CountsOf(const SyncDiff& d, const std::string& node)
{
    const auto it = d.row_counts.find(node);
    return it == d.row_counts.end() ? LibSyncCounts() : it->second;
}

LibSyncCounts TotalCounts(const SyncDiff& d)
{
    LibSyncCounts n;
    n.here = d.here;
    n.file = d.file;
    n.both = d.both;
    return n;
}

// ---- Apply -----------------------------------------------------------------------------------------------------

namespace {

// One variation cluster's frame in each original text, by its exact `start` token.
struct VarRef {
    bool        has[4] = {false, false, false, false};
    std::string start[4];
};

struct ApplyCtx {
    std::set<std::string>         x_keys;      // every Exists key of the diff
    std::set<std::string>         case_nodes;  // nodes the diff lists Case changes of
    std::map<std::string, VarRef> var;         // V key -> its frames
};

void CopyNodeRecord(NodeDoc* dst, const NodeDoc& src, bool with_cases)
{
    *dst = src;
    if (!with_cases) {
        dst->cases.clear();
        dst->unassigned.clear();
    }
}

void RemoveSubtree(Doc& d, const std::string& uid)
{
    const std::set<std::string> gone = SubtreeIn(d, uid);
    d.nodes.erase(std::remove_if(d.nodes.begin(), d.nodes.end(), [&](const NodeDoc& n) { return gone.count(n.uid) > 0; }), d.nodes.end());
    d.Reindex();
}

// Where a node of `src` goes in `dst`: after the nearest earlier sibling `dst` has (and that sibling's subtree), else
// before its first sibling, else right after its parent. Returns the vector index to insert at.
size_t InsertPos(const Doc& dst, const Doc& src, const std::string& uid, const std::string& parent)
{
    const std::vector<std::string> sib = ChildrenIn(src, parent);
    size_t                         me = 0;
    for (size_t i = 0; i < sib.size(); ++i)
        if (sib[i] == uid) me = i;
    for (size_t i = me; i-- > 0;)
        if (const NodeDoc* p = dst.N(sib[i])) {
            const std::set<std::string> sub = SubtreeIn(dst, p->uid);
            size_t                      at = static_cast<size_t>(p - &dst.nodes[0]) + 1;
            while (at < dst.nodes.size() && sub.count(dst.nodes[at].uid)) ++at;
            return at;
        }
    const std::vector<std::string> mine = ChildrenIn(dst, parent);
    if (!mine.empty()) return static_cast<size_t>(dst.N(mine[0]) - &dst.nodes[0]);
    if (const NodeDoc* pn = dst.N(parent)) return static_cast<size_t>(pn - &dst.nodes[0]) + 1;
    return dst.nodes.size();
}

void EnsureSound(NodeDoc& n)
{
    if (!n.has_sound) {
        n.has_sound = true;
        n.sound = Line{"sound", {}};
    }
}

int FrameByStart(const NodeDoc& n, const std::string& start)
{
    for (size_t i = 0; i < n.frames.size(); ++i)
        if (TokOr(n.frames[i].frame, "start") == start) return static_cast<int>(i);
    return -1;
}

// Copies one key's value from `src` into `dst` (roles: which original text each one started from). True when `dst`
// now holds src's value for that key (false: nothing to hang it on, e.g. a node whose parent is not there).
bool CopyKey(Doc& dst, int dst_role, const Doc& src, int src_role, const std::string& key, const ApplyCtx& cx)
{
    const std::vector<std::string> p = Split(key, '|');
    const std::string&             kind = p[0];
    if (kind == "X") {
        const std::string& u = p[1];
        const NodeDoc*     sn = src.N(u);
        if (!sn) {
            // Second review M1: a node of the removed subtree that src still has (moved elsewhere there) is kept, under
            // src's parent; it goes with the subtree only when that parent is not there to hang it on.
            const std::set<std::string> sub = SubtreeIn(dst, u);
            bool                        moved = false;
            for (NodeDoc& n : dst.nodes) {
                if (n.uid == u || !sub.count(n.uid)) continue;
                const NodeDoc* s = src.N(n.uid);
                if (!s || s->parent.empty() || s->parent == n.parent) continue;
                if (!sub.count(s->parent) && !dst.N(s->parent)) continue;
                CopyTok(n.node, s->node, "parent");
                n.parent = s->parent;
                moved = true;
            }
            if (moved) dst.Reindex();
            RemoveSubtree(dst, u);
            return true;
        }
        if (dst.N(u)) return true;
        if (!sn->parent.empty() && !dst.N(sn->parent)) return false;  // its parent is not there: nothing to hang it on
        std::vector<std::string> order = {u};
        std::function<void(const std::string&)> walk = [&](const std::string& x) {
            for (const std::string& c : ChildrenIn(src, x))
                if (!dst.N(c) && !cx.x_keys.count(KeyOf({"X", c}))) {
                    order.push_back(c);
                    walk(c);
                }
        };
        walk(u);
        size_t at = InsertPos(dst, src, u, sn->parent);
        for (const std::string& x : order) {
            NodeDoc nd;
            CopyNodeRecord(&nd, *src.N(x), !cx.case_nodes.count(x));
            dst.nodes.insert(dst.nodes.begin() + static_cast<long>(at), std::move(nd));
            ++at;
        }
        dst.Reindex();
        return true;
    }
    if (kind == "C") {
        NodeDoc* dn = dst.N(p[1]);
        if (!dn) return false;
        const NodeDoc* sn = src.N(p[1]);
        dn->cases.erase(std::remove_if(dn->cases.begin(), dn->cases.end(), [&](const Line& l) { return TokOr(l, "value") == p[2]; }), dn->cases.end());
        if (sn)
            for (const Line& l : sn->cases)
                if (TokOr(l, "value") == p[2]) dn->cases.push_back(l);
        return true;
    }
    if (kind == "F" || kind == "P" || kind == "PK" || kind == "XL" || kind == "U" || kind == "V" || kind == "O") {
        NodeDoc*       dn = dst.N(p[1]);
        const NodeDoc* sn = src.N(p[1]);
        if (!dn || !sn) return false;
        if (kind == "F" && p[2] == "node") {
            CopyTok(dn->node, sn->node, p[3]);
            if (p[3] == "parent") dn->parent = TokOr(dn->node, "parent");
        } else if (kind == "F") {
            if (!sn->has_sound && !dn->has_sound) return true;
            EnsureSound(*dn);
            if (p[3] == "fades") {
                for (const char* k : {"fin", "fout"}) {
                    if (sn->has_sound) CopyTok(dn->sound, sn->sound, k);
                    else EraseTok(dn->sound, k);
                }
            } else if (sn->has_sound) {
                CopyTok(dn->sound, sn->sound, p[3]);
            } else {
                EraseTok(dn->sound, p[3]);
            }
        } else if (kind == "P") {
            if (!sn->has_sound && !dn->has_sound) return true;
            EnsureSound(*dn);
            for (const char* k : kPartKeys) {
                if (sn->has_sound) CopyTok(dn->sound, sn->sound, k);
                else EraseTok(dn->sound, k);
            }
            dn->parts = sn->has_sound ? sn->parts : std::vector<Line>();
            dn->zones = sn->has_sound ? sn->zones : std::vector<Line>();
        } else if (kind == "PK") {
            dn->parked = sn->parked;
        } else if (kind == "XL") {
            dn->extra = sn->extra;
        } else if (kind == "U") {
            dn->unassigned = sn->unassigned;
        } else if (kind == "V") {
            const auto vr = cx.var.find(key);
            if (vr == cx.var.end()) return false;
            if (vr->second.has[dst_role]) {
                const int di = FrameByStart(*dn, vr->second.start[dst_role]);
                if (di >= 0) dn->frames.erase(dn->frames.begin() + di);
            }
            if (vr->second.has[src_role]) {
                const int si = FrameByStart(*sn, vr->second.start[src_role]);
                if (si < 0) return false;
                EnsureSound(*dn);
                const Var& v = sn->frames[static_cast<size_t>(si)];
                size_t     at = 0;
                while (at < dn->frames.size() && FrameStart(dn->frames[at]) <= FrameStart(v)) ++at;
                dn->frames.insert(dn->frames.begin() + static_cast<long>(at), v);
            }
        } else if (kind == "O") {
            const std::vector<std::string> mine = ChildrenIn(dst, p[1]);
            const std::vector<std::string> theirs = ChildrenIn(src, p[1]);
            std::vector<std::string>       common;  // src order of the uids both have
            for (const std::string& u : theirs)
                if (std::find(mine.begin(), mine.end(), u) != mine.end()) common.push_back(u);
            std::vector<size_t> slots;  // indices in dst.nodes of the children that are common
            for (const std::string& u : mine)
                if (std::find(common.begin(), common.end(), u) != common.end()) slots.push_back(static_cast<size_t>(dst.N(u) - &dst.nodes[0]));
            if (slots.size() != common.size()) return false;
            std::vector<NodeDoc> recs;
            for (const std::string& u : common) recs.push_back(*dst.N(u));
            for (size_t i = 0; i < slots.size(); ++i) dst.nodes[slots[i]] = recs[i];
            dst.Reindex();
        }
        return true;
    }
    if (kind == "GX") {
        const GroupDoc* sg = src.G(p[1]);
        if (!sg) return false;
        if (dst.G(p[1])) return true;
        GroupDoc g = *sg;
        g.values.clear();
        for (const Line& v : sg->values)
            if (!cx.x_keys.count(KeyOf({"VX", p[1], TokOr(v, "uid")}))) g.values.push_back(v);
        dst.groups.push_back(g);
        return true;
    }
    if (kind == "GF" || kind == "GE") {
        GroupDoc*       dg = dst.G(p[1]);
        const GroupDoc* sg = src.G(p[1]);
        if (!dg || !sg) return false;
        if (kind == "GF") CopyTok(dg->group, sg->group, p[2]);
        else dg->extra = sg->extra;
        return true;
    }
    if (kind == "VX" || kind == "VF") {
        const GroupDoc* sg = src.G(p[1]);
        if (!sg) return false;
        GroupDoc* dg = dst.G(p[1]);
        if (!dg) {
            GroupDoc g = *sg;
            g.values.clear();
            dst.groups.push_back(g);
            dg = &dst.groups.back();
        }
        const Line* sv = ValueLine(*sg, p[2]);
        if (kind == "VX") {
            if (!sv) return false;
            if (!ValueLine(*dg, p[2])) dg->values.push_back(*sv);
            return true;
        }
        for (Line& l : dg->values)
            if (TokOr(l, "uid") == p[2] && sv) CopyTok(l, *sv, p[3]);
        return true;
    }
    if (kind == "LF") {
        CopyTok(dst.lib, src.lib, p[1]);
    } else if (kind == "LK") {
        dst.has_kit = src.has_kit;
        dst.kit = src.kit;
    } else if (kind == "LE") {
        dst.lib_extra = src.lib_extra;
    }
    return true;
}

// Review M2: a node both sides added goes into the base on its own (its record only; its parent must be there).
void FoldNodeOnly(Doc& dst, const Doc& src, const std::string& uid)
{
    const NodeDoc* sn = src.N(uid);
    if (!sn || dst.N(uid) || (!sn->parent.empty() && !dst.N(sn->parent))) return;
    const size_t at = InsertPos(dst, src, uid, sn->parent);
    dst.nodes.insert(dst.nodes.begin() + static_cast<long>(at), *sn);
    dst.Reindex();
}

// Does any line of `d` but the definitions refer to `uid` (a value or a group)?
bool Referenced(const Doc& d, const std::string& uid)
{
    auto in_tok = [&](const std::string& tok) {
        const size_t e = EqPos(tok);
        if (e == std::string::npos) return false;
        std::string key = tok.substr(0, e);
        if (key == "uid") return false;
        if (key.compare(0, 4, "pdb.") == 0 && key.substr(4) == uid) return true;
        for (const std::string& a : Split(tok.substr(e + 1), ','))
            for (const std::string& b : Split(a, ':'))
                if (b == uid) return true;
        return false;
    };
    auto in_line = [&](const Line& l) {
        for (const std::string& t : l.toks)
            if (in_tok(t)) return true;
        return false;
    };
    auto in_lines = [&](const std::vector<Line>& v) {
        for (const Line& l : v)
            if (in_line(l)) return true;
        return false;
    };
    for (const GroupDoc& g : d.groups)
        if (in_line(g.group)) return true;
    for (const NodeDoc& n : d.nodes) {
        if (in_line(n.node) || in_lines(n.cases) || in_lines(n.unassigned) || in_lines(n.parts) || in_lines(n.zones) ||
            in_lines(n.parked) || in_lines(n.extra))
            return true;
        if (n.has_sound && in_line(n.sound)) return true;
        for (const Var& v : n.frames)
            if (in_line(v.frame) || in_lines(v.fz)) return true;
    }
    return false;
}

// Review M4: Send mine of a value / group only the file has drops it from the file when nothing there uses it.
void DropUnusedFromFile(Doc& f, const std::string& key)
{
    const std::vector<std::string> p = Split(key, '|');
    GroupDoc*                      g = f.G(p[1]);
    if (!g) return;
    auto drop_value = [&](const std::string& v) {
        if (Referenced(f, v)) return false;
        g->values.erase(std::remove_if(g->values.begin(), g->values.end(), [&](const Line& l) { return TokOr(l, "uid") == v; }), g->values.end());
        return true;
    };
    if (p[0] == "VX") {
        drop_value(p[2]);
        return;
    }
    for (const Line& v : std::vector<Line>(g->values)) drop_value(TokOr(v, "uid"));
    if (g->values.empty() && !Referenced(f, p[1]))
        f.groups.erase(std::remove_if(f.groups.begin(), f.groups.end(), [&](const GroupDoc& x) { return x.uid == p[1]; }), f.groups.end());
}

// Every group / value a node or case of `d` refers to is there (copied whole from `from` when not).
void EnsureGroups(Doc& d, const std::vector<const Doc*>& from)
{
    auto have_group = [&](const std::string& g) { return d.G(g) != nullptr; };
    auto add_group = [&](const std::string& g) {
        for (const Doc* s : from)
            if (s && s->G(g)) {
                d.groups.push_back(*s->G(g));
                return;
            }
    };
    auto add_value = [&](const std::string& v) {
        for (const GroupDoc& g : d.groups)
            if (ValueLine(g, v)) return;
        for (const Doc* s : from)
            if (s)
                for (const GroupDoc& g : s->groups)
                    if (const Line* l = ValueLine(g, v)) {
                        if (!d.G(g.uid)) {
                            GroupDoc ng = g;
                            ng.values.clear();
                            d.groups.push_back(ng);
                        }
                        d.G(g.uid)->values.push_back(*l);
                        return;
                    }
    };
    for (const NodeDoc& n : d.nodes) {
        const std::string g = TokOr(n.node, "group");
        if (!g.empty() && !have_group(g)) add_group(g);
        if (n.has_sound) {
            const std::string dv = TokOr(n.sound, "div");
            if (!dv.empty() && !have_group(dv)) add_group(dv);
            for (const std::string& v : Split(TokOr(n.sound, "divv"), ','))
                if (!v.empty()) add_value(v);
        }
        for (const Line& c : n.cases) add_value(TokOr(c, "value"));
        for (const char* k : {"default", "preview"}) {
            const std::string v = TokOr(n.node, k);
            if (!v.empty()) add_value(v);
        }
    }
}

// A group / value of `dst` made present or absent as in `src` (the file base following the file, second review H1).
void MatchPresence(Doc& dst, const Doc& src, const std::string& key)
{
    const std::vector<std::string> p = Split(key, '|');
    const GroupDoc*                sg = src.G(p[1]);
    GroupDoc*                      dg = dst.G(p[1]);
    if (p[0] == "GX") {
        if (!sg) dst.groups.erase(std::remove_if(dst.groups.begin(), dst.groups.end(), [&](const GroupDoc& x) { return x.uid == p[1]; }), dst.groups.end());
        else if (!dg) dst.groups.push_back(*sg);
        return;
    }
    const Line* sv = sg ? ValueLine(*sg, p[2]) : nullptr;
    if (!sv) {
        if (dg) dg->values.erase(std::remove_if(dg->values.begin(), dg->values.end(), [&](const Line& l) { return TokOr(l, "uid") == p[2]; }), dg->values.end());
        return;
    }
    if (!dg) {
        GroupDoc g = *sg;
        g.values.clear();
        dst.groups.push_back(g);
        dg = &dst.groups.back();
    }
    if (!ValueLine(*dg, p[2])) dg->values.push_back(*sv);
}

// Review H1 / L3: a node `orig` had whose parent this Apply changed, when that parent is not there or the change makes a
// loop, keeps its old parent; its move key is reported (it stays open).
void KeepOldParents(Doc& dst, const Doc& orig, std::set<std::string>* reverted)
{
    for (size_t guard = 0; guard <= dst.nodes.size() + 1; ++guard) {
        dst.Reindex();
        std::string bad;
        for (const NodeDoc& n : dst.nodes) {
            const NodeDoc* o = orig.N(n.uid);
            if (!o || o->parent == n.parent) continue;
            if (!n.parent.empty() && !dst.N(n.parent)) {
                bad = n.uid;
                break;
            }
            std::string u = n.parent;
            for (size_t k = 0; k <= dst.nodes.size() && !u.empty() && u != n.uid; ++k) {
                const NodeDoc* pn = dst.N(u);
                u = pn ? pn->parent : std::string();
            }
            if (u == n.uid) {
                bad = n.uid;
                break;
            }
        }
        if (bad.empty()) return;
        NodeDoc*       n = dst.N(bad);
        const NodeDoc* o = orig.N(bad);
        CopyTok(n->node, o->node, "parent");
        n->parent = o->parent;
        if (reverted) reverted->insert(KeyOf({"F", bad, "node", "parent"}));
    }
}

void Sanitize(Doc& d)
{
    bool again = true;
    while (again) {
        again = false;
        d.Reindex();
        for (size_t i = 0; i < d.nodes.size(); ++i) {
            const NodeDoc& n = d.nodes[i];
            if (!n.parent.empty() && !d.N(n.parent)) {
                d.nodes.erase(d.nodes.begin() + static_cast<long>(i));
                again = true;
                break;
            }
        }
    }
    for (NodeDoc& n : d.nodes) {
        n.cases.erase(std::remove_if(n.cases.begin(), n.cases.end(), [&](const Line& l) { return !d.N(TokOr(l, "child")); }), n.cases.end());
        n.unassigned.erase(std::remove_if(n.unassigned.begin(), n.unassigned.end(), [&](const Line& l) { return !d.N(TokOr(l, "child")); }), n.unassigned.end());
    }
}

}  // namespace

bool ApplySync(const std::string& base_text, const std::string& mine_text, const std::string& file_text,
               const SyncDiff& diff, const std::map<std::string, SyncDir>& picks, SyncResult* out)
{
    return ApplySync(base_text, base_text, mine_text, file_text, diff, picks, out);
}

bool ApplySync(const std::string& base_text, const std::string& file_base_text, const std::string& mine_text,
               const std::string& file_text, const SyncDiff& diff, const std::map<std::string, SyncDir>& picks,
               SyncResult* out)
{
    Doc m0, f0, b0, fb0;
    if (!ParseDoc(mine_text, &m0) || !ParseDoc(file_text, &f0)) return false;
    const bool known = !base_text.empty() && !file_base_text.empty() && ParseDoc(base_text, &b0) && ParseDoc(file_base_text, &fb0);
    Doc        m1 = m0, f1 = f0, b1 = b0;
    ApplyCtx   cx;
    Docs       ds;
    ds.d[kB] = known ? &b0 : nullptr;
    ds.d[kM] = &m0;
    ds.d[kF] = &f0;
    ds.d[kFB] = known ? &fb0 : nullptr;
    std::map<std::string, std::vector<Cluster>> clusters;
    auto note_var = [&](const std::string& key) {
        const std::vector<std::string> p = Split(key, '|');
        if (p.size() < 3 || p[0] != "V") return;
        if (!clusters.count(p[1])) clusters[p[1]] = ClustersOf(ds, p[1]);
        for (const Cluster& c : clusters[p[1]]) {
            if (c.id != p[2]) continue;
            VarRef r;
            for (int di = 0; di < 4; ++di)
                if (ds.at(di) && c.member[di] >= 0) {
                    r.has[di] = true;
                    r.start[di] = TokOr(ds.at(di)->N(p[1])->frames[static_cast<size_t>(c.member[di])].frame, "start");
                }
            cx.var[key] = r;
        }
    };
    for (const SyncChange& c : diff.changes) {
        if (c.key.compare(0, 2, "X|") == 0 || c.key.compare(0, 3, "VX|") == 0 || c.key.compare(0, 3, "GX|") == 0) cx.x_keys.insert(c.key);
        if (c.key.compare(0, 2, "C|") == 0) cx.case_nodes.insert(Split(c.key, '|')[1]);
        note_var(c.key);
    }
    for (const std::string& k : diff.folds) note_var(k);
    for (const std::string& k : diff.file_folds) note_var(k);
    // order: groups / values first, additions parents first, the rest, removals children first
    struct Op {
        std::string key;
        SyncDir     dir;
        int         phase, depth;
        bool        applied = false;
        bool        lone_absent = false;  // a group / value the chosen side does not have
    };
    std::vector<Op> ops;
    for (const auto& pk : picks) {
        const SyncChange* c = diff.Find(pk.first);
        if (!c) continue;
        const std::vector<std::string> p = Split(pk.first, '|');
        int                            phase = 2, depth = c->depth;
        if (p[0] == "GX" || p[0] == "VX") phase = 0;
        else if (p[0] == "X") {
            const Doc& src = pk.second == SyncDir::Up ? m0 : f0;
            const bool add = src.N(p[1]) != nullptr;
            phase = add ? 1 : 3;
            depth = add ? c->depth : -c->depth;
        }
        Op op;
        op.key = pk.first;
        op.dir = pk.second;
        op.phase = phase;
        op.depth = depth;
        ops.push_back(op);
    }
    std::stable_sort(ops.begin(), ops.end(), [](const Op& a, const Op& b) { return a.phase != b.phase ? a.phase < b.phase : a.depth < b.depth; });
    for (Op& op : ops) {
        const bool                     up = op.dir == SyncDir::Up;
        const Doc&                     src = up ? m0 : f0;
        const std::vector<std::string> p = Split(op.key, '|');
        if (p[0] == "GX" || p[0] == "VX") {
            const bool has = p[0] == "GX" ? src.G(p[1]) != nullptr : (src.G(p[1]) && ValueLine(*src.G(p[1]), p[2]));
            if (!has) {  // chosen absent: never removed from the project; dropped from the file when unused there
                op.lone_absent = true;
                op.applied = true;
                continue;
            }
        }
        op.applied = up ? CopyKey(f1, kF, src, kM, op.key, cx) : CopyKey(m1, kM, src, kF, op.key, cx);
    }
    // a node a side already had is never dropped by a move (review H1 / L3)
    std::set<std::string> reverted;
    KeepOldParents(m1, m0, &reverted);
    KeepOldParents(f1, f0, &reverted);
    int sent = 0, taken = 0;
    std::vector<std::string> open;
    for (Op& op : ops) {
        if (op.applied && reverted.count(op.key)) op.applied = false;
        if (!op.applied) {
            open.push_back(op.key);
            continue;
        }
        (op.dir == SyncDir::Up ? sent : taken)++;
    }
    EnsureGroups(m1, {&f0, &m0, &b0});
    Sanitize(m1);
    Sanitize(f1);
    // L3: a value / group Send mine drops from the file goes once every other change is in, so a case of the file that
    // this Apply removes no longer keeps it there; one the file still uses stays (in its place)
    for (const Op& op : ops)
        if (op.lone_absent && op.dir == SyncDir::Up) DropUnusedFromFile(f1, op.key);
    EnsureGroups(f1, {&m1, &m0, &f0, &b0});
    Doc fb1 = fb0;
    if (known) {  // the base takes each applied change's chosen value, and every key equal on both sides
        for (const Op& op : ops) {
            if (!op.applied) continue;
            const bool up = op.dir == SyncDir::Up;
            if (op.lone_absent) {
                if (!up) CopyKey(b1, kB, m0, kM, op.key, cx);  // the project keeps it: the base follows the project
                continue;
            }
            CopyKey(b1, kB, up ? m0 : f0, up ? kM : kF, op.key, cx);
        }
        // second review H1: the file base, key by key: the applied changes and the keys where the file already equals
        // the project; every other key keeps the file base it had (an unpicked change of the file stays listed)
        for (const Op& op : ops) {
            if (!op.applied) continue;
            const bool up = op.dir == SyncDir::Up;
            if (op.lone_absent) MatchPresence(fb1, f1, op.key);
            else CopyKey(fb1, kFB, up ? m0 : f0, up ? kM : kF, op.key, cx);
        }
        std::vector<std::string> ffolds = diff.file_folds;
        std::stable_sort(ffolds.begin(), ffolds.end(), [&](const std::string& a, const std::string& b) {
            const bool xa = a.compare(0, 2, "X|") == 0, xb = b.compare(0, 2, "X|") == 0;
            if (xa != xb) return xa;
            return xa && DepthOf(diff.parent, a.substr(2)) < DepthOf(diff.parent, b.substr(2));
        });
        for (const std::string& k : ffolds) {
            if (k.compare(0, 2, "X|") == 0 && f0.N(k.substr(2))) FoldNodeOnly(fb1, f0, k.substr(2));
            else CopyKey(fb1, kFB, f0, kF, k, cx);
        }
        EnsureGroups(fb1, {&f1, &f0, &m1, &m0});
        KeepOldParents(fb1, fb0, nullptr);
        Sanitize(fb1);
        std::vector<std::string> folds = diff.folds;
        std::stable_sort(folds.begin(), folds.end(), [&](const std::string& a, const std::string& b) {
            const bool xa = a.compare(0, 2, "X|") == 0, xb = b.compare(0, 2, "X|") == 0;
            if (xa != xb) return xa;
            return xa && DepthOf(diff.parent, a.substr(2)) < DepthOf(diff.parent, b.substr(2));
        });
        for (const std::string& k : folds) {
            if (k.compare(0, 2, "X|") == 0 && m0.N(k.substr(2))) FoldNodeOnly(b1, m0, k.substr(2));
            else CopyKey(b1, kB, m0, kM, k, cx);
        }
        EnsureGroups(b1, {&m1, &f1, &m0, &f0});
        KeepOldParents(b1, b0, nullptr);
        Sanitize(b1);
    }
    SyncResult r;
    r.sent = sent;
    r.taken = taken;
    r.open = open;
    r.mine = WriteDoc(m1);
    r.file = WriteDoc(f1);
    r.mine_changed = r.mine != WriteDoc(m0);
    r.file_changed = r.file != WriteDoc(f0);
    if (known) {
        r.base = WriteDoc(b1);
        r.file_base = WriteDoc(fb1);
    } else {
        bool all = open.empty();
        for (const SyncChange& c : diff.changes) all = all && picks.count(c.key) > 0;
        if (all) {  // everything settled: the common versions are the result
            r.base = r.mine;
            r.file_base = r.file;
        }
    }
    *out = std::move(r);
    return true;
}

bool CarryChanges(const std::string& old_text, const std::string& new_text, const std::string& onto, std::string* out)
{
    if (old_text.empty() || new_text.empty() || onto.empty()) return false;
    const SyncDiff                 d = DiffLibrary(old_text, old_text, new_text, onto);
    std::map<std::string, SyncDir> picks;
    for (const SyncChange& c : d.changes)
        if (c.side != SyncSide::File) picks[c.key] = SyncDir::Up;
    SyncResult r;
    if (!ApplySync(old_text, old_text, new_text, onto, d, CloseSelection(d, picks), &r)) return false;
    *out = r.file;
    return true;
}

SyncRoute RouteSyncAnswer(bool tab_open, bool tab_shown)
{
    if (!tab_open) return SyncRoute::Drop;
    return tab_shown ? SyncRoute::Handle : SyncRoute::Hold;
}

// ---- uid translation --------------------------------------------------------------------------------------------

std::string TranslateUidsText(const std::string& text, const std::vector<std::pair<std::string, std::string>>& uid_map,
                              bool to_project)
{
    if (uid_map.empty()) return text;
    std::map<std::string, std::string> m;
    for (const auto& kv : uid_map) {
        if (to_project) m[kv.first] = kv.second;
        else m[kv.second] = kv.first;
    }
    auto sub = [&](const std::string& s) {  // runs of non-separators
        std::string out;
        size_t      i = 0;
        while (i < s.size()) {
            if (s[i] == ',' || s[i] == ':') {
                out += s[i++];
                continue;
            }
            size_t j = i;
            while (j < s.size() && s[j] != ',' && s[j] != ':') ++j;
            const std::string run = s.substr(i, j - i);
            auto              it = m.find(run);
            out += it == m.end() ? run : it->second;
            i = j;
        }
        return out;
    };
    std::string out;
    size_t      pos = 0;
    bool        first_line = true;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        const bool has_nl = nl != std::string::npos;
        if (!has_nl) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (first_line) {  // the header is left alone
            first_line = false;
            out += line + (has_nl ? "\n" : "");
            continue;
        }
        bool        cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        std::string res;
        size_t      lp = 0;
        bool        first_tok = true;
        while (lp <= line.size()) {
            size_t sp = line.find(' ', lp);
            if (sp == std::string::npos) sp = line.size();
            std::string tok = line.substr(lp, sp - lp);
            const size_t e = tok.find('=');
            if (!first_tok && e != std::string::npos && e > 0) {
                std::string key = tok.substr(0, e);
                if (key.compare(0, 4, "pdb.") == 0) key = "pdb." + sub(key.substr(4));
                tok = key + "=" + sub(tok.substr(e + 1));
            }
            res += (first_tok ? "" : " ") + tok;
            first_tok = false;
            lp = sp + 1;
        }
        out += res + (cr ? "\r" : "") + (has_nl ? "\n" : "");
    }
    return out;
}

// ---- the store side ---------------------------------------------------------------------------------------------

bool ReplaceLibraryFromMerge(SoundStore& store, const std::string& lib_uid, const SoundStore& merged, const SoundStore* before)
{
    SoundLibrary* lib = FindLibrary(store, lib_uid);
    if (!lib || merged.libraries.size() != 1) return false;
    SoundLibrary nl = merged.libraries.front();
    nl.uid = lib->uid;
    nl.ravlib_path = lib->ravlib_path;
    nl.kit_offered = lib->kit_offered;
    nl.user = false;
    nl.attached = lib->attached;
    nl.attached_type = lib->attached_type;
    for (SoundNode& n : nl.nodes) {
        if (n.type != SoundNodeType::Sound || n.sound.source.kind != SoundSourceKind::File) continue;
        if (const SoundNode* old = FindNode(*lib, n.uid)) n.sound.source.item_guid = old->sound.source.item_guid;
    }
    if (nl.name != lib->name) {  // a name another library of the project holds stays unique (13-77a's clash rename)
        std::vector<std::string> taken;
        for (const SoundLibrary& l : store.libraries)
            if (l.uid != lib_uid) taken.push_back(l.name);
        nl.name = UniqueName(nl.name, taken);
    }
    *lib = std::move(nl);
    for (const SwitchGroup& mg : merged.groups) {
        SwitchGroup* g = FindGroup(store, mg.uid);
        if (!g) {
            store.groups.push_back(mg);
            continue;
        }
        // a group of the project this library did not use: only the values it lacks come (its name, default and
        // values stand)
        const bool used_before = !before || FindGroup(*before, mg.uid) != nullptr;
        if (used_before) {
            g->name = mg.name;
            g->default_value = mg.default_value;
            g->rav_owned = mg.rav_owned || g->rav_owned;
            g->kept = mg.kept;
            g->kept_lines = mg.kept_lines;
        }
        for (const SwitchValue& mv : mg.values) {
            bool found = false;
            for (SwitchValue& v : g->values)
                if (v.uid == mv.uid) {
                    if (used_before) v = mv;
                    found = true;
                }
            if (found) continue;
            SwitchValue nv = mv;  // 13-23: the project's role map stands; only the keys no value of the group holds come
            std::vector<std::string> roles;
            for (const std::string& r : nv.roles) {
                bool held = false;
                for (const SwitchValue& pv : g->values) held = held || std::find(pv.roles.begin(), pv.roles.end(), r) != pv.roles.end();
                if (!held) roles.push_back(r);
            }
            nv.roles = roles;
            g->values.push_back(nv);
        }
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> NameMergeMap(const std::vector<SwitchGroup>& project_groups,
                                                              const std::string& file_proj_text,
                                                              const std::vector<std::pair<std::string, std::string>>& uid_map)
{
    std::vector<std::pair<std::string, std::string>> out;
    Doc                                              d;
    if (!ParseDoc(file_proj_text, &d)) return out;
    std::set<std::string> taken;  // project uids that must not be matched: the file uses them, or a file uid maps there
    for (const auto& kv : uid_map) taken.insert(kv.second);
    std::set<std::string> mapped_from;
    for (const auto& kv : uid_map) mapped_from.insert(kv.first);
    for (const GroupDoc& g : d.groups) {
        taken.insert(g.uid);
        for (const Line& v : g.values) taken.insert(TokOr(v, "uid"));
    }
    for (const GroupDoc& g : d.groups) {
        const SwitchGroup* pg = nullptr;
        for (const SwitchGroup& x : project_groups)
            if (x.uid == g.uid) pg = &x;
        if (!pg && !mapped_from.count(g.uid)) {
            const std::string key = SwitchNameKey(PctDecode(TokOr(g.group, "name")));
            for (const SwitchGroup& x : project_groups)
                if (!taken.count(x.uid) && SwitchNameKey(x.name) == key) {
                    pg = &x;
                    out.push_back({g.uid, x.uid});
                    taken.insert(x.uid);
                    break;
                }
        }
        if (!pg) continue;
        for (const Line& v : g.values) {
            const std::string vu = TokOr(v, "uid");
            bool              have = false;
            for (const SwitchValue& pv : pg->values) have = have || pv.uid == vu;
            if (have || mapped_from.count(vu)) continue;
            const std::string key = SwitchNameKey(PctDecode(TokOr(v, "name")));
            for (const SwitchValue& pv : pg->values)
                if (!taken.count(pv.uid) && SwitchNameKey(pv.name) == key) {
                    out.push_back({vu, pv.uid});
                    taken.insert(pv.uid);
                    break;
                }
        }
    }
    return out;
}

// ---- gates ------------------------------------------------------------------------------------------------------

uint64_t SyncDiffSignature(const SyncDiff& d)
{
    std::string s = d.root;
    for (const SyncChange& c : d.changes)
        s += "\n" + c.key + (c.side == SyncSide::Here ? "h" : c.side == SyncSide::File ? "f" : "b") + c.node + "\x1f" + c.text + "\x1f" + c.here_text + "\x1f" + c.file_text;
    return SyncTextHash(s);
}

uint64_t SyncTextHash(const std::string& text)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h == 0 ? 1 : h;
}

ApplyGate DecideApplyGate(const SyncFileStamp& listed, const SyncFileStamp& now)
{
    if (now.size < 0) return listed.size < 0 ? ApplyGate::Go : ApplyGate::FileGone;
    if (listed.size < 0) return ApplyGate::FileChanged;
    return listed == now ? ApplyGate::Go : ApplyGate::FileChanged;
}

JournalStep DecideJournal(const SyncJournal& j, uint64_t base_now, bool file_exists, uint64_t file_hash)
{
    const uint64_t after = SyncTextHash(j.after), before = SyncTextHash(j.before);
    if (base_now == j.base_before && j.base_before != j.base_after) {
        if (file_exists && file_hash == after) return JournalStep::RestoreBefore;
        if (!file_exists || file_hash != before) return JournalStep::FileWrittenSince;
        return JournalStep::None;  // the file is already back
    }
    if (base_now == j.base_after && j.base_before != j.base_after && file_exists && file_hash == before) return JournalStep::RestoreAfter;
    return JournalStep::None;
}

uint64_t LinkStateHash(const LinkBase& b)
{
    return SyncTextHash(b.text + '\x1e' + b.file_base + '\x1e' + std::to_string(b.syncs));
}

LinkBase NextLinkBase(const LinkBase* old, const std::string& lib_uid, const SyncResult& r,
                      const std::vector<std::pair<std::string, std::string>>& uid_map, int64_t now,
                      const std::string& file_by, int64_t file_at)
{
    LinkBase nb = old ? *old : LinkBase();
    nb.lib_uid = lib_uid;
    if (!r.base.empty()) {
        nb.text = r.base;
        nb.file_base = r.file_base;
    }
    nb.uid_map = uid_map;
    nb.syncs = (old ? old->syncs : 0) + 1;
    nb.synced_at = now;
    nb.file_by = file_by;
    nb.file_at = file_at;
    return nb;
}

namespace {

// The newest Apply not undone (-1: all undone).
int NewestApplied(const std::vector<bool>& undone)
{
    for (int i = static_cast<int>(undone.size()) - 1; i >= 0; --i)
        if (!undone[static_cast<size_t>(i)]) return i;
    return -1;
}

// Undo: the newest k <= newest applied whose state before is the state now. Redo: the newest q >= oldest undone whose
// state after is the state now. -1 = none.
int UndoTarget(const std::vector<SyncJournal>& js, const std::vector<bool>& undone, uint64_t state_now)
{
    for (int k = NewestApplied(undone); k >= 0; --k)
        if (js[static_cast<size_t>(k)].base_before == state_now) return k;
    return -1;
}

int RedoTarget(const std::vector<SyncJournal>& js, const std::vector<bool>& undone, uint64_t state_now)
{
    const int q0 = NewestApplied(undone) + 1;
    for (int q = static_cast<int>(js.size()) - 1; q >= q0; --q)
        if (js[static_cast<size_t>(q)].base_after == state_now) return q;
    return -1;
}

// Every Apply in [a, b] started from the file the previous one wrote (nobody wrote it between them).
bool Chained(const std::vector<SyncJournal>& js, int a, int b)
{
    for (int i = a + 1; i <= b; ++i)
        if (js[static_cast<size_t>(i)].before != js[static_cast<size_t>(i - 1)].after) return false;
    return true;
}

}  // namespace

bool JournalChainCandidate(const std::vector<SyncJournal>& js, const std::vector<bool>& undone, uint64_t state_now,
                           bool undo_names_sync, bool redo_names_sync)
{
    if (js.size() != undone.size()) return false;
    if (undo_names_sync && UndoTarget(js, undone, state_now) >= 0) return true;
    return redo_names_sync && RedoTarget(js, undone, state_now) >= 0;
}

JournalPlan DecideJournalChain(const std::vector<SyncJournal>& js, const std::vector<bool>& undone, uint64_t state_now,
                               bool undo_names_sync, bool redo_names_sync, bool file_exists, uint64_t file_hash)
{
    JournalPlan plan;
    plan.undone = undone;
    if (js.size() != undone.size()) return plan;
    const int p = NewestApplied(undone);
    if (undo_names_sync) {
        const int k = UndoTarget(js, undone, state_now);
        if (k >= 0) {
            for (int i = k; i <= p; ++i) plan.undone[static_cast<size_t>(i)] = true;
            const SyncJournal& newest = js[static_cast<size_t>(p)];
            if (file_exists && file_hash == SyncTextHash(js[static_cast<size_t>(k)].before)) return plan;  // already back
            if (file_exists && file_hash == SyncTextHash(newest.after) && Chained(js, k, p)) {
                plan.step = JournalStep::RestoreBefore;
                plan.write = js[static_cast<size_t>(k)].before;
                return plan;
            }
            plan.step = JournalStep::FileWrittenSince;
            plan.drop = true;
            return plan;
        }
    }
    if (redo_names_sync) {
        const int q = RedoTarget(js, undone, state_now);
        if (q >= 0) {
            const int q0 = p + 1;
            for (int i = q0; i <= q; ++i) plan.undone[static_cast<size_t>(i)] = false;
            if (file_exists && file_hash == SyncTextHash(js[static_cast<size_t>(q)].after)) return plan;  // already forward
            if (file_exists && file_hash == SyncTextHash(js[static_cast<size_t>(q0)].before) && Chained(js, q0, q)) {
                plan.step = JournalStep::RestoreAfter;
                plan.write = js[static_cast<size_t>(q)].after;
                return plan;
            }
            plan.undone = undone;  // the file moved: leave it alone, only the project is forward
            return plan;
        }
    }
    return plan;
}

// ---- texts ------------------------------------------------------------------------------------------------------

std::string SyncTagText(const LibSyncCounts& c)
{
    const int n = c.Total();
    if (n == 0) return std::string();
    std::string s = c.both > 0 || (c.here > 0 && c.file > 0) ? "both" : c.here > 0 ? "edited" : "in global";
    if (n > 1) s += " · " + std::to_string(n);
    return s;
}

std::string SyncTagTip(const LibSyncCounts& c)
{
    const int n = c.Total();
    if (n == 0) return std::string();
    const char* what = c.both > 0 || (c.here > 0 && c.file > 0) ? "Changed here and in the global library"
                       : c.here > 0                               ? "Changed here, not in the global library yet"
                                                                  : "Changed in the global library since this project last synced";
    return std::string(what) + ": " + std::to_string(n) + (n == 1 ? " change" : " changes") + ". Click to see what";
}

std::string SyncRowUpText(const LibSyncCounts& c)
{
    const int n = c.here + c.both;
    return n > 0 ? "\xE2\x86\x91" + std::to_string(n) : std::string();
}

std::string SyncRowDownText(const LibSyncCounts& c)
{
    const int n = c.file + c.both;
    return n > 0 ? "\xE2\x86\x93" + std::to_string(n) : std::string();
}

std::string SyncCountLine(int n, SyncSide side)
{
    const std::string c = std::to_string(n) + (n == 1 ? " change" : " changes");
    if (side == SyncSide::Here) return c + " made here, not in the global library";
    if (side == SyncSide::File) return c + " in the global library, not here";
    return c + " changed on both sides";
}

std::string SyncRowTip(const LibSyncCounts& c)
{
    std::string s;
    if (c.here > 0) s += SyncCountLine(c.here, SyncSide::Here) + "\n";
    if (c.file > 0) s += SyncCountLine(c.file, SyncSide::File) + "\n";
    if (c.both > 0) s += SyncCountLine(c.both, SyncSide::Both) + "\n";
    return s + "Click to open Sync with global";
}

std::string SyncAppliedToast(int sent, int taken, const std::string& file, int open)
{
    std::string s = "Applied: ";
    if (sent > 0) s += std::to_string(sent) + " written to " + file + " (the version before is kept as " + file + ".bak)";
    if (sent > 0 && taken > 0) s += ", ";
    if (taken > 0) s += std::to_string(taken) + " taken here";
    if (sent == 0 && taken == 0) s += "nothing to do";
    if (open > 0) s += " \xC2\xB7 " + std::to_string(open) + (open == 1 ? " could not be applied: it stays listed" : " could not be applied: they stay listed");
    return s;
}

}  // namespace rav
