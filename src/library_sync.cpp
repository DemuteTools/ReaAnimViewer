// SPDX-License-Identifier: MIT
//
// See library_sync.h. Spec 13-77b.

#include "library_sync.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "sound_record.h"

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

struct Trip {
    Opt b, m, f;
};

struct Docs {
    const Doc* b = nullptr;  // null = no common version
    const Doc* m = nullptr;
    const Doc* f = nullptr;
    const Doc* at(int i) const { return i == 0 ? b : i == 1 ? m : f; }
};

// Variation clusters of one node: frames of the three texts matched by start within 1 ms.
struct Cluster {
    long long ms = 0;
    int       member[3] = {-1, -1, -1};  // frame index per text (b, m, f)
};

std::vector<Cluster> ClustersOf(const Docs& ds, const std::string& uid)
{
    struct Item {
        double start;
        int    doc, idx;
    };
    std::vector<Item> items;
    for (int di = 0; di < 3; ++di) {
        const Doc* d = ds.at(di);
        if (!d) continue;
        const NodeDoc* n = d->N(uid);
        if (!n) continue;
        for (size_t i = 0; i < n->frames.size(); ++i) items.push_back({FrameStart(n->frames[i]), di, static_cast<int>(i)});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.start < b.start; });
    std::vector<Cluster> cl;
    double               last = -1e18;
    for (const Item& it : items) {
        if (cl.empty() || it.start - last > 0.00105) {
            Cluster c;
            c.ms = std::llround(it.start * 1000.0);
            cl.push_back(c);
        }
        Cluster& c = cl.back();
        if (c.member[it.doc] < 0) c.member[it.doc] = it.idx;
        last = it.start;
    }
    return cl;
}

int FindFrame(const NodeDoc& n, long long ms)
{
    for (size_t i = 0; i < n.frames.size(); ++i)
        if (std::fabs(FrameStart(n.frames[i]) * 1000.0 - static_cast<double>(ms)) <= 1.6) return static_cast<int>(i);
    return -1;
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

struct Describe {
    const Docs& ds;

    std::string NodeName(const std::string& uid) const
    {
        for (int i : {1, 2, 0}) {
            const std::string n = NameIn(ds.at(i), uid);
            if (!n.empty()) return n;
        }
        return uid;
    }
    const NodeDoc* AnyNode(const std::string& uid) const
    {
        for (int i : {1, 2, 0})
            if (ds.at(i) && ds.at(i)->N(uid)) return ds.at(i)->N(uid);
        return nullptr;
    }
    std::string ValueName(const std::string& vuid) const
    {
        for (int i : {1, 2, 0}) {
            const Doc* d = ds.at(i);
            if (!d) continue;
            for (const GroupDoc& g : d->groups)
                if (const Line* l = ValueLine(g, vuid)) return PctDecode(TokOr(*l, "name", vuid));
        }
        return vuid;
    }
    std::string GroupName(const std::string& guid) const
    {
        for (int i : {1, 2, 0}) {
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

Cls Classify(const Trip& t, bool had_base, bool untouched)
{
    if (t.m == t.f) return Cls::None;
    if (!had_base) return Cls::Both;
    const Opt& ef = untouched ? t.b : t.f;
    if (t.m == ef) return Cls::None;
    if (t.m == t.b) return Cls::File;
    if (ef == t.b) return Cls::Here;
    return Cls::Both;
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
    bool b = false, m = false, f = false;
};

Opt MakeOpt(const std::string* v)
{
    Opt o;
    if (v) o.has = true, o.v = *v;
    return o;
}

}  // namespace

const SyncChange* SyncDiff::Find(const std::string& key) const
{
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

}  // namespace

SyncDiff DiffLibrary(const std::string& base_text, const std::string& mine_text, const std::string& file_text,
                     bool file_untouched)
{
    SyncDiff out;
    Doc      db, dm, df;
    if (!ParseDoc(mine_text, &dm) || !ParseDoc(file_text, &df)) return out;
    const bool had_base = !base_text.empty() && ParseDoc(base_text, &db);
    out.had_base = had_base;
    out.file_untouched = had_base && file_untouched;
    out.root = dm.RootUid();
    Docs ds;
    ds.b = had_base ? &db : nullptr;
    ds.m = &dm;
    ds.f = &df;
    Describe desc{ds};

    // union tree
    for (int i : {1, 2, 0}) {
        const Doc* d = ds.at(i);
        if (!d) continue;
        for (const NodeDoc& n : d->nodes)
            if (!out.parent.count(n.uid)) out.parent[n.uid] = n.parent;
    }
    for (const NodeDoc& n : dm.nodes) out.mine_nodes.insert(n.uid);

    // triples
    std::map<std::string, Trip> trips;
    {
        KV kb, km, kf;
        if (had_base) Collect(db, &kb);
        Collect(dm, &km);
        Collect(df, &kf);
        for (const auto& kv : kb) trips[kv.first].b = MakeOpt(&kv.second);
        for (const auto& kv : km) trips[kv.first].m = MakeOpt(&kv.second);
        for (const auto& kv : kf) trips[kv.first].f = MakeOpt(&kv.second);
    }
    std::map<std::string, Pres> pres;
    for (int i = 0; i < 3; ++i) {
        const Doc* d = ds.at(i);
        if (!d) continue;
        for (const NodeDoc& n : d->nodes) {
            Pres& p = pres[n.uid];
            (i == 0 ? p.b : i == 1 ? p.m : p.f) = true;
        }
    }
    // variations and order
    std::map<std::string, std::vector<Cluster>> var_clusters;
    for (const auto& pp : pres) {
        const std::string& u = pp.first;
        if (!((pp.second.m && pp.second.f) || (had_base && pp.second.b && (pp.second.m || pp.second.f)))) continue;
        const std::vector<Cluster> cl = ClustersOf(ds, u);
        var_clusters[u] = cl;
        for (const Cluster& c : cl) {
            const std::string key = KeyOf({"V", u, std::to_string(c.ms)});
            Trip&             t = trips[key];
            for (int di = 0; di < 3; ++di) {
                const Doc* d = ds.at(di);
                if (!d || c.member[di] < 0) continue;
                const NodeDoc* n = d->N(u);
                Opt            o;
                o.has = true;
                o.v = VarValue(n->frames[c.member[di]]);
                (di == 0 ? t.b : di == 1 ? t.m : t.f) = o;
            }
        }
        // order of the children present everywhere
        std::vector<std::string> kids[3];
        for (int di = 0; di < 3; ++di) {
            const Doc* d = ds.at(di);
            if (!d) continue;
            for (const std::string& c : ChildrenIn(*d, u)) {
                const Pres& cp = pres[c];
                if (cp.m && cp.f && (!had_base || cp.b)) kids[di].push_back(c);
            }
        }
        if (kids[1].size() >= 2) {
            Trip& t = trips[KeyOf({"O", u})];
            t.m = Opt{true, Join(kids[1], ",")};
            t.f = Opt{true, Join(kids[2], ",")};
            if (had_base) t.b = Opt{true, Join(kids[0], ",")};
        }
    }

    // the group / value universe for needs
    auto key_node = [](const std::string& key) {
        const std::vector<std::string> p = Split(key, '|');
        return p.size() > 1 ? p[1] : std::string();
    };

    // count of changed keys of a node subtree between the base and one side (decision 4)
    auto subtree_changes = [&](const std::string& top, const Doc& holder, bool file_side) {
        std::set<std::string> sub;
        std::function<void(const std::string&)> walk = [&](const std::string& u) {
            sub.insert(u);
            for (const std::string& c : ChildrenIn(holder, u)) walk(c);
        };
        walk(top);
        int n = 0;
        for (const auto& kv : trips) {
            const std::vector<std::string> p = Split(kv.first, '|');
            if (p.size() < 2 || !NodeScoped(p[0]) || !sub.count(p[1])) continue;
            const Opt& side = file_side ? kv.second.f : kv.second.m;
            if (side != kv.second.b) ++n;
        }
        return n;
    };

    const bool untouched = out.file_untouched;
    std::map<std::string, Cls> cls_of;
    auto add = [&](SyncChange c, Cls cl) {
        c.side = SideOf(cl);
        cls_of[c.key] = cl;
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
            const bool         pf = untouched ? pr.b : pr.f;
            if (pr.m == pf) {
                if (pr.m && pr.f && !pr.b && had_base) {}  // both added the same uid: equal presence
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
            Cls               cl;
            const bool        sound = type == "sound";
            if (!had_base) {
                cl = Cls::Both;
                c.here_text = pr.m ? "present" : "absent";
                c.file_text = pr.f ? "present" : "absent";
                c.text = TypeWord(type) + " " + nm + ": here " + c.here_text + " · file " + c.file_text;
                c.node = sound ? out.parent[u] : u;
                c.label = nm;
                add(c, cl);
                continue;
            }
            if (pr.b) {  // removed by the side that lacks it
                const bool mine_removed = !pr.m;
                const Doc* lack = mine_removed ? &dm : &df;
                const Doc* keep = mine_removed ? &df : &dm;
                const std::string par = out.parent[u];
                if (par.empty() || !lack->N(par)) continue;  // not the topmost removed
                const int n = subtree_changes(u, *keep, mine_removed);
                const int sounds = desc.SoundsUnder(keep, u);
                const std::string tail = !sound && sounds > 0 ? " (" + std::to_string(sounds) + (sounds == 1 ? " sound)" : " sounds)") : std::string();
                if (n > 0) {
                    cl = Cls::Both;
                    c.here_text = mine_removed ? "removed" : std::to_string(n) + (n == 1 ? " change" : " changes");
                    c.file_text = mine_removed ? std::to_string(n) + (n == 1 ? " change" : " changes") : "removed";
                    c.text = TypeWord(type) + " " + nm + ": here " + c.here_text + " · file " + c.file_text;
                } else {
                    cl = mine_removed ? Cls::Here : Cls::File;
                    c.here_text = mine_removed ? "removed" : "present";
                    c.file_text = mine_removed ? "present" : "removed";
                    c.text = TypeWord(type) + " " + nm + " removed" + tail;
                }
            } else {  // added by the side that has it
                cl = pr.m ? Cls::Here : Cls::File;
                c.here_text = pr.m ? "present" : "absent";
                c.file_text = pr.f ? "present" : "absent";
                c.text = TypeWord(type) + " " + nm + " added";
            }
            c.label = nm;
            if (sound) c.node = out.parent[u];
            add(c, cl);
            continue;
        }
        if (kind == "GX" || kind == "VX") {
            if (!had_base) continue;
            if (t.m == t.f || t.b.has) continue;
            const bool mh = t.m.has, fh = t.f.has;
            if (mh == fh) continue;
            if (untouched && fh) continue;
            SyncChange c;
            c.key = key;
            c.node = "grp:" + p[1];
            const Cls cl = mh ? Cls::Here : Cls::File;
            if (kind == "GX") {
                c.text = "Switch Group " + desc.GroupName(p[1]) + " added";
                c.label = desc.GroupName(p[1]);
            } else {
                c.text = "Value " + desc.ValueName(p[2]) + " added";
                c.label = desc.ValueName(p[2]);
            }
            c.here_text = mh ? "present" : "absent";
            c.file_text = fh ? "present" : "absent";
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
                if ((!pr.m || !pr.f) && had_base && pr.b) continue;  // under a removed node
            } else if (!(pr.m && pr.f)) {
                continue;
            }
        }
        const Cls cl = Classify(t, had_base, untouched);
        if (cl == Cls::None) {
            if (had_base && t.m == t.f && t.m != t.b) out.folds.push_back(key);
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
        if (kind == "F") {
            const Shown sh = ShowField(desc, p[2], p[3], t.b, t.m);  // labels / units
            const Shown mh = ShowField(desc, p[2], p[3], t.b, t.m);
            const Shown fh = ShowField(desc, p[2], p[3], t.b, t.f);
            c.label = sh.label;
            c.here_text = mh.to + mh.unit;
            c.file_text = fh.to + fh.unit;
            if (mh.rename) {
                c.text = cl == Cls::Both ? "Renamed: here " + mh.to + " · file " + fh.to : "Renamed " + (cl == Cls::Here ? mh.from : fh.from) + " → " + (cl == Cls::Here ? mh.to : fh.to);
            } else if (p[3] != "name" && sh.label.size() > 8 && sh.label.compare(sh.label.size() - 8, 8, " changed") == 0) {
                c.text = of(sh.label);
                if (sound_subject) c.text = sh.label + " (" + subject + ")";
            } else if (cl == Cls::Both) {
                c.text = of(sh.label) + ": here " + c.here_text + " · file " + c.file_text;
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
            c.file_text = "file order";
        } else if (kind == "V") {
            const std::vector<Cluster>& cl2 = var_clusters[p[1]];
            int                         ix = 0, k = 0;
            for (const Cluster& cc : cl2) {
                ++k;
                if (std::to_string(cc.ms) == p[2]) ix = k;
            }
            c.label = "Variation " + std::to_string(ix);
            const std::string vn = "Variation " + std::to_string(ix) + " of " + subject;
            c.here_text = t.m.has ? "present" : "absent";
            c.file_text = t.f.has ? "present" : "absent";
            const Opt& side = cl == Cls::File ? t.f : t.m;
            if (cl == Cls::Both) c.text = vn + ": here " + (t.m.has ? "changed" : "removed") + " · file " + (t.f.has ? "changed" : "removed");
            else if (!side.has) c.text = vn + " removed";
            else if (!t.b.has) c.text = vn + " added";
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
            if (cl == Cls::Both) c.text = "Switch value " + vname + ": here " + c.here_text + " · file " + c.file_text;
            else if (!side.has) c.text = "Switch value " + vname + " removed";
            else if (!t.b.has) c.text = "Switch value " + vname + " added (plays " + childname(side) + ")";
            else c.text = "Switch value " + vname + " now plays " + childname(side);
        } else if (kind == "GF") {
            const Shown mh = ShowField(desc, "group", p[2], t.b, t.m), fh = ShowField(desc, "group", p[2], t.b, t.f);
            c.label = mh.label;
            c.here_text = mh.to;
            c.file_text = fh.to;
            c.text = "Switch Group " + desc.GroupName(p[1]) + ": " + mh.label + (cl == Cls::Both ? ": here " + mh.to + " · file " + fh.to : cl == Cls::Here ? " " + mh.from + " → " + mh.to : " " + fh.from + " → " + fh.to);
        } else if (kind == "GE") {
            c.label = "Other settings";
            c.text = "Other settings of Switch Group " + desc.GroupName(p[1]) + " changed";
        } else if (kind == "VF") {
            const Shown mh = ShowField(desc, "value", p[3], t.b, t.m), fh = ShowField(desc, "value", p[3], t.b, t.f);
            c.label = p[3] == "roles" ? "Roles" : mh.label;
            c.here_text = mh.to;
            c.file_text = fh.to;
            c.text = "Value " + desc.ValueName(p[2]) + ": " + c.label + (cl == Cls::Both ? ": here " + mh.to + " · file " + fh.to : cl == Cls::Here ? " " + mh.from + " → " + mh.to : " " + fh.from + " → " + fh.to);
        } else if (kind == "LF") {
            const Shown mh = ShowField(desc, "lib", p[1], t.b, t.m), fh = ShowField(desc, "lib", p[1], t.b, t.f);
            c.label = p[1] == "name" ? "Library name" : p[1] == "color" ? "Colour" : mh.label;
            c.here_text = mh.to;
            c.file_text = fh.to;
            if (p[1] == "name")
                c.text = cl == Cls::Both ? "Renamed library: here " + mh.to + " · file " + fh.to : "Renamed library " + (cl == Cls::Here ? mh.from : fh.from) + " → " + (cl == Cls::Here ? mh.to : fh.to);
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
    for (SyncChange& c : out.changes) {
        if (c.side == SyncSide::Both) continue;
        const bool        here = c.side == SyncSide::Here;
        const Doc*        sd = here ? &dm : &df;  // the doc that holds the change
        const std::vector<std::string> p = Split(c.key, '|');
        if (p[0] == "X") {
            const std::string& u = p[1];
            const Pres&        pr = pres[u];
            const bool added = !pr.b || !had_base;
            if (added) {
                need(c, KeyOf({"X", out.parent[u]}));
            } else {  // removal: the cases that play it go too
                for (const auto& kv : trips) {
                    const std::vector<std::string> q = Split(kv.first, '|');
                    if (q[0] != "C" || !kv.second.b.has) continue;
                    if (TokOr(ParseLine("case " + kv.second.b.v), "child") == u) need(c, kv.first);
                }
            }
        } else if (p[0] == "C") {
            const Trip& t = trips[c.key];
            const Opt&  side = here ? t.m : t.f;
            need(c, KeyOf({"X", p[1]}));
            if (side.has) {
                const std::string child = TokOr(ParseLine("case " + side.v), "child");
                need(c, KeyOf({"X", child}));
                const std::string g = group_of_value(sd, p[2]);
                if (!g.empty()) need(c, KeyOf({"VX", g, p[2]}));
            }
        } else if (p[0] == "VX") {
            need(c, KeyOf({"GX", p[1]}));
        } else if (p[0] == "U" || p[0] == "F" || p[0] == "P" || p[0] == "V") {
            need(c, KeyOf({"X", p[1]}));
        }
    }

    for (const SyncChange& c : out.changes) {
        if (c.side == SyncSide::Here) ++out.here;
        else if (c.side == SyncSide::File) ++out.file;
        else ++out.both;
    }
    std::sort(out.folds.begin(), out.folds.end());
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
        for (const SyncChange& c : d.changes)
            if (std::find(c.needs.begin(), c.needs.end(), k) != c.needs.end() && seen.insert(c.key).second) todo.push_back(c.key);
    }
    seen.erase(key);
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
        for (const std::string& k : adopt ? NeedsClosure(d, pk.first) : DependentsClosure(d, pk.first)) out[k] = pk.second;
    }
    return out;
}

std::vector<std::string> ContainerChangeKeys(const SyncDiff& d, const std::string& container)
{
    std::vector<std::string> keys;
    for (const SyncChange& c : d.changes) {
        bool in = c.node == container;
        if (!in && c.node.compare(0, 4, "grp:") != 0 && !d.mine_nodes.count(c.node)) {
            std::string u = c.node;
            for (int i = 0; i < 1000 && !u.empty() && !in; ++i) {
                auto it = d.parent.find(u);
                if (it == d.parent.end()) break;
                u = it->second;
                if (u == container) in = true;
                else if (d.mine_nodes.count(u)) break;
            }
        }
        if (in) keys.push_back(c.key);
    }
    return keys;
}

LibSyncCounts CountsOf(const SyncDiff& d, const std::string& node)
{
    LibSyncCounts n;
    for (const SyncChange& c : d.changes)
        if (c.node == node) (c.side == SyncSide::Here ? n.here : c.side == SyncSide::File ? n.file : n.both)++;
    return n;
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

struct ApplyCtx {
    std::set<std::string> x_keys;      // every Exists key of the diff
    std::set<std::string> case_nodes;  // nodes the diff lists Case changes of
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
    std::set<std::string> gone;
    std::function<void(const std::string&)> walk = [&](const std::string& u) {
        gone.insert(u);
        for (const std::string& c : ChildrenIn(d, u))
            if (!gone.count(c)) walk(c);
    };
    walk(uid);
    d.nodes.erase(std::remove_if(d.nodes.begin(), d.nodes.end(), [&](const NodeDoc& n) { return gone.count(n.uid) > 0; }), d.nodes.end());
    d.Reindex();
}

// Where a node of `src` goes in `dst`: after the nearest earlier sibling `dst` has, else before its first sibling,
// else at the end. Returns the vector index to insert at.
size_t InsertPos(const Doc& dst, const Doc& src, const std::string& uid, const std::string& parent)
{
    const std::vector<std::string> sib = ChildrenIn(src, parent);
    size_t                         me = 0;
    for (size_t i = 0; i < sib.size(); ++i)
        if (sib[i] == uid) me = i;
    for (size_t i = me; i-- > 0;)
        if (const NodeDoc* p = dst.N(sib[i])) {
            // after the sibling's own record and its subtree records that directly follow
            size_t at = static_cast<size_t>(p - &dst.nodes[0]) + 1;
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

// Copies one key's value from `src` into `dst`. `src_side_is_dst` is never passed.
void CopyKey(Doc& dst, const Doc& src, const std::string& key, const ApplyCtx& cx)
{
    const std::vector<std::string> p = Split(key, '|');
    const std::string&             kind = p[0];
    if (kind == "X") {
        const std::string& u = p[1];
        const NodeDoc*     sn = src.N(u);
        if (!sn) {
            RemoveSubtree(dst, u);
            return;
        }
        if (dst.N(u)) return;
        if (!sn->parent.empty() && !dst.N(sn->parent)) return;  // its parent is not there: nothing to hang it on
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
        return;
    }
    if (kind == "C") {
        NodeDoc* dn = dst.N(p[1]);
        if (!dn) return;
        const NodeDoc* sn = src.N(p[1]);
        dn->cases.erase(std::remove_if(dn->cases.begin(), dn->cases.end(), [&](const Line& l) { return TokOr(l, "value") == p[2]; }), dn->cases.end());
        if (sn)
            for (const Line& l : sn->cases)
                if (TokOr(l, "value") == p[2]) dn->cases.push_back(l);
        return;
    }
    if (kind == "F" || kind == "P" || kind == "PK" || kind == "XL" || kind == "U" || kind == "V" || kind == "O") {
        NodeDoc*       dn = dst.N(p[1]);
        const NodeDoc* sn = src.N(p[1]);
        if (!dn || !sn) return;
        if (kind == "F" && p[2] == "node") {
            CopyTok(dn->node, sn->node, p[3]);
            if (p[3] == "parent") dn->parent = TokOr(dn->node, "parent");
        } else if (kind == "F") {
            if (!sn->has_sound && !dn->has_sound) return;
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
            if (!sn->has_sound && !dn->has_sound) return;
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
            const long long ms = std::atoll(p[2].c_str());
            const int       di = FindFrame(*dn, ms);
            const int       si = FindFrame(*sn, ms);
            if (di >= 0) dn->frames.erase(dn->frames.begin() + di);
            if (si >= 0) {
                EnsureSound(*dn);
                const Var& v = sn->frames[si];
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
            if (slots.size() != common.size()) return;
            std::vector<NodeDoc> recs;
            for (const std::string& u : common) recs.push_back(*dst.N(u));
            for (size_t i = 0; i < slots.size(); ++i) dst.nodes[slots[i]] = recs[i];
            dst.Reindex();
        }
        return;
    }
    if (kind == "GX") {
        const GroupDoc* sg = src.G(p[1]);
        if (!sg || dst.G(p[1])) return;
        GroupDoc g = *sg;
        g.values.clear();
        for (const Line& v : sg->values)
            if (!cx.x_keys.count(KeyOf({"VX", p[1], TokOr(v, "uid")}))) g.values.push_back(v);
        dst.groups.push_back(g);
        return;
    }
    if (kind == "GF" || kind == "GE") {
        GroupDoc*       dg = dst.G(p[1]);
        const GroupDoc* sg = src.G(p[1]);
        if (!dg || !sg) return;
        if (kind == "GF") CopyTok(dg->group, sg->group, p[2]);
        else dg->extra = sg->extra;
        return;
    }
    if (kind == "VX" || kind == "VF") {
        const GroupDoc* sg = src.G(p[1]);
        if (!sg) return;
        GroupDoc* dg = dst.G(p[1]);
        if (!dg) {
            GroupDoc g = *sg;
            g.values.clear();
            dst.groups.push_back(g);
            dg = &dst.groups.back();
        }
        const Line* sv = ValueLine(*sg, p[2]);
        if (kind == "VX") {
            if (!sv || ValueLine(*dg, p[2])) return;
            dg->values.push_back(*sv);
            return;
        }
        for (Line& l : dg->values)
            if (TokOr(l, "uid") == p[2] && sv) CopyTok(l, *sv, p[3]);
        return;
    }
    if (kind == "LF") {
        CopyTok(dst.lib, src.lib, p[1]);
    } else if (kind == "LK") {
        dst.has_kit = src.has_kit;
        dst.kit = src.kit;
    } else if (kind == "LE") {
        dst.lib_extra = src.lib_extra;
    }
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
    Doc m0, f0, b0;
    if (!ParseDoc(mine_text, &m0) || !ParseDoc(file_text, &f0)) return false;
    const bool had_base = !base_text.empty() && ParseDoc(base_text, &b0);
    Doc        m1 = m0, f1 = f0, b1 = b0;
    ApplyCtx   cx;
    for (const SyncChange& c : diff.changes) {
        if (c.key.compare(0, 2, "X|") == 0 || c.key.compare(0, 3, "VX|") == 0 || c.key.compare(0, 3, "GX|") == 0) cx.x_keys.insert(c.key);
        if (c.key.compare(0, 2, "C|") == 0) cx.case_nodes.insert(Split(c.key, '|')[1]);
    }
    // order: groups / values first, additions parents first, the rest, removals children first
    struct Op {
        std::string key;
        SyncDir     dir;
        int         phase, depth;
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
        ops.push_back({pk.first, pk.second, phase, depth});
    }
    std::stable_sort(ops.begin(), ops.end(), [](const Op& a, const Op& b) { return a.phase != b.phase ? a.phase < b.phase : a.depth < b.depth; });
    int sent = 0, taken = 0;
    for (const Op& op : ops) {
        const bool        up = op.dir == SyncDir::Up;
        const Doc&        src = up ? m0 : f0;
        const std::string kind = Split(op.key, '|')[0];
        (up ? sent : taken)++;
        const bool lone_group = kind == "GX" || kind == "VX";
        if (lone_group) {
            const std::vector<std::string> p = Split(op.key, '|');
            const bool                     has = kind == "GX" ? src.G(p[1]) != nullptr : (src.G(p[1]) && ValueLine(*src.G(p[1]), p[2]));
            if (!has) {  // chosen absent: a group / value is never removed from the project, the base keeps it as is
                if (had_base) CopyKey(b1, m0, op.key, cx);
                continue;
            }
        }
        if (up) CopyKey(f1, src, op.key, cx);
        else CopyKey(m1, src, op.key, cx);
        if (had_base) CopyKey(b1, src, op.key, cx);
    }
    if (had_base)
        for (const std::string& k : diff.folds) CopyKey(b1, m0, k, cx);
    EnsureGroups(m1, {&f0, &m0, &b0});
    EnsureGroups(f1, {&m1, &m0, &f0, &b0});
    if (had_base) EnsureGroups(b1, {&m1, &f1, &m0, &f0});
    Sanitize(m1);
    Sanitize(f1);
    if (had_base) Sanitize(b1);
    SyncResult r;
    r.sent = sent;
    r.taken = taken;
    r.mine = WriteDoc(m1);
    r.file = WriteDoc(f1);
    r.mine_changed = r.mine != WriteDoc(m0);
    r.file_changed = r.file != WriteDoc(f0);
    if (had_base) r.base = WriteDoc(b1);
    else if (picks.size() >= diff.changes.size() && !diff.changes.empty()) r.base = WriteDoc(m1);
    else if (diff.changes.empty()) r.base = r.mine;
    *out = std::move(r);
    return true;
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

bool ReplaceLibraryFromMerge(SoundStore& store, const std::string& lib_uid, const SoundStore& merged)
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
    *lib = std::move(nl);
    for (const SwitchGroup& mg : merged.groups) {
        SwitchGroup* g = FindGroup(store, mg.uid);
        if (!g) {
            store.groups.push_back(mg);
            continue;
        }
        g->name = mg.name;
        g->default_value = mg.default_value;
        g->rav_owned = mg.rav_owned || g->rav_owned;
        g->kept = mg.kept;
        g->kept_lines = mg.kept_lines;
        for (const SwitchValue& mv : mg.values) {
            bool found = false;
            for (SwitchValue& v : g->values)
                if (v.uid == mv.uid) {
                    v = mv;
                    found = true;
                }
            if (!found) g->values.push_back(mv);
        }
    }
    return true;
}

// ---- gates ------------------------------------------------------------------------------------------------------

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

// ---- texts ------------------------------------------------------------------------------------------------------

std::string SyncTagText(const LibSyncCounts& c)
{
    const int n = c.Total();
    if (n == 0) return std::string();
    std::string s = c.both > 0 || (c.here > 0 && c.file > 0) ? "both" : c.here > 0 ? "edited" : "in file";
    if (n > 1) s += " · " + std::to_string(n);
    return s;
}

std::string SyncTagTip(const LibSyncCounts& c)
{
    const int n = c.Total();
    if (n == 0) return std::string();
    const char* what = c.both > 0 || (c.here > 0 && c.file > 0) ? "Changed here and in the file"
                       : c.here > 0                               ? "Changed here, not in the file yet"
                                                                  : "Changed in the file since this project last synced";
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
    if (side == SyncSide::Here) return c + " made here, not in the file";
    if (side == SyncSide::File) return c + " in the file, not here";
    return c + " changed on both sides";
}

std::string SyncRowTip(const LibSyncCounts& c)
{
    std::string s;
    if (c.here > 0) s += SyncCountLine(c.here, SyncSide::Here) + "\n";
    if (c.file > 0) s += SyncCountLine(c.file, SyncSide::File) + "\n";
    if (c.both > 0) s += SyncCountLine(c.both, SyncSide::Both) + "\n";
    return s + "Click to open Sync with file";
}

std::string SyncAppliedToast(int sent, int taken, const std::string& file)
{
    std::string s = "Applied: ";
    if (sent > 0) s += std::to_string(sent) + " written to " + file + " (the version before is kept as " + file + ".bak)";
    if (sent > 0 && taken > 0) s += ", ";
    if (taken > 0) s += std::to_string(taken) + " taken here";
    if (sent == 0 && taken == 0) s += "nothing to do";
    return s;
}

}  // namespace rav
