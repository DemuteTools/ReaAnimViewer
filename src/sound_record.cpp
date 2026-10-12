// SPDX-License-Identifier: MIT
//
// See sound_record.h.

#include "sound_record.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <system_error>
#include <utility>

namespace rav {
namespace {

using Fields = std::vector<std::pair<std::string, std::string>>;
using Kept = std::vector<std::string>;

enum class Got { Ok, Unknown, Bad };

Got G(bool ok)
{
    return ok ? Got::Ok : Got::Bad;
}

// ---- Values ---------------------------------------------------------------------------------

bool NeedsEscape(unsigned char c)
{
    return c <= 0x20 || c == 0x7F || c == ',' || c == '=' || c == '%';
}

std::string Enc(const std::string& s)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string       out;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (NeedsEscape(c)) {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        } else {
            out += ch;
        }
    }
    return out;
}

int Hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool Dec(const std::string& s, std::string* out)
{
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size()) return false;
            const int h = Hex(s[i + 1]), l = Hex(s[i + 2]);
            if (h < 0 || l < 0) return false;
            r += static_cast<char>(h * 16 + l);
            i += 2;
        } else {
            r += s[i];
        }
    }
    *out = r;
    return true;
}

template <typename T>
std::string FmtInt(T v)
{
    char                       buf[32];
    const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

template <typename T>
bool ReadInt(const std::string& s, T* out)
{
    if (s.empty()) return false;
    T                            v{};
    const char*                  b = s.data();
    const char*                  e = b + s.size();
    const std::from_chars_result r = std::from_chars(b, e, v);
    if (r.ec != std::errc() || r.ptr != e) return false;
    *out = v;
    return true;
}

bool ReadFlag(const std::string& s, bool* out)
{
    if (s == "0") *out = false;
    else if (s == "1") *out = true;
    else return false;
    return true;
}

bool ReadStr(const std::string& s, std::string* out)
{
    return Dec(s, out);
}

std::string FmtColor(uint32_t c)
{
    static const char kHex[] = "0123456789abcdef";
    std::string       s;
    for (int i = 28; i >= 0; i -= 4) s += kHex[(c >> i) & 15];
    return s;
}

bool ReadColor(const std::string& s, uint32_t* out)
{
    if (s.empty() || s.size() > 8) return false;
    uint32_t v = 0;
    for (char ch : s) {
        const int h = Hex(ch);
        if (h < 0) return false;
        v = v * 16 + static_cast<uint32_t>(h);
    }
    *out = v;
    return true;
}

// ---- Lines ----------------------------------------------------------------------------------

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    size_t                   pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string l = text.substr(pos, nl - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        pos = nl + 1;
    }
    return lines;
}

std::vector<std::string> Tokens(const std::string& rest)
{
    std::vector<std::string> t;
    if (rest.empty()) return t;
    size_t pos = 0;
    while (true) {
        size_t sp = rest.find(' ', pos);
        if (sp == std::string::npos) {
            t.push_back(rest.substr(pos));
            break;
        }
        t.push_back(rest.substr(pos, sp - pos));
        pos = sp + 1;
    }
    return t;
}

// Reads the line's tokens through `set`; what it does not take is kept as written.
void ReadFields(const std::vector<std::string>& toks, size_t from, Kept& kept,
                const std::function<Got(const std::string&, const std::string&)>& set)
{
    kept.clear();
    for (size_t i = from; i < toks.size(); ++i) {
        const std::string& tok = toks[i];
        const size_t       eq = tok.find('=');
        if (eq == std::string::npos || eq == 0) {
            kept.push_back(tok);
            continue;
        }
        if (set(tok.substr(0, eq), tok.substr(eq + 1)) != Got::Ok) kept.push_back(tok);
    }
}

// A known field whose value did not read is kept as written: it stands for that field (the
// model's value for it is not written next to it).
bool KeptHasKey(const Kept& kept, const std::string& key)
{
    for (const std::string& k : kept)
        if (k.size() > key.size() && k.compare(0, key.size(), key) == 0 && k[key.size()] == '=') return true;
    return false;
}

void WriteLine(std::string& out, const char* kind, const Fields& f, const Kept& kept)
{
    out += kind;
    for (const auto& kv : f)
        if (!KeptHasKey(kept, kv.first)) out += ' ' + kv.first + '=' + kv.second;
    for (const std::string& k : kept) out += ' ' + k;
    out += '\n';
}

void WriteLines(std::string& out, const Kept& lines)
{
    for (const std::string& l : lines) out += l + '\n';
}

// The header: "<magic> <n>" then fields. False when the text does not start with it.
bool ReadHeader(const std::vector<std::string>& lines, const char* magic, size_t* line_i, int* version,
                std::vector<std::string>* rest)
{
    size_t i = 0;
    while (i < lines.size()) {
        std::string l = lines[i];
        if (i == 0 && l.size() >= 3 && static_cast<unsigned char>(l[0]) == 0xEF &&
            static_cast<unsigned char>(l[1]) == 0xBB && static_cast<unsigned char>(l[2]) == 0xBF)
            l = l.substr(3);
        if (l.find_first_not_of(" \t") == std::string::npos) {
            ++i;
            continue;
        }
        const std::vector<std::string> t = Tokens(l);
        if (t.size() < 2 || t[0] != magic) return false;
        int v = 0;
        if (!ReadInt(t[1], &v) || v < 1) return false;
        *version = v;
        rest->assign(t.begin() + 2, t.end());
        *line_i = i + 1;
        return true;
    }
    return false;
}

std::string KindOf(const std::string& l, std::vector<std::string>* toks)
{
    *toks = Tokens(l);
    return toks->empty() ? std::string() : (*toks)[0];
}

// ---- Words ------------------------------------------------------------------------------------

const char* const kNodeWords[] = {"sound", "random", "blend", "switch"};
const char* const kOwnWords[] = {"populated", "attached", "spread", "deleted"};
const char* const kModeWords[] = {"whole", "takes", "tmarkers", "silence"};

bool ModeFromWord(const std::string& s, DetectMode* out)
{
    for (int i = 0; i < 4; ++i)
        if (s == kModeWords[i]) {
            *out = static_cast<DetectMode>(i);
            return true;
        }
    return false;
}

// ---- Store: fields ----------------------------------------------------------------------------

Fields GroupFields(const SwitchGroup& g)
{
    return {{"uid", Enc(g.uid)}, {"name", Enc(g.name)}};
}

Got SetGroup(SwitchGroup& g, const std::string& k, const std::string& v)
{
    if (k == "uid") return G(ReadStr(v, &g.uid));
    if (k == "name") return G(ReadStr(v, &g.name));
    return Got::Unknown;
}

Fields ValueFields(const SwitchValue& x)
{
    return {{"uid", Enc(x.uid)}, {"name", Enc(x.name)}};
}

Got SetValue(SwitchValue& x, const std::string& k, const std::string& v)
{
    if (k == "uid") return G(ReadStr(v, &x.uid));
    if (k == "name") return G(ReadStr(v, &x.name));
    return Got::Unknown;
}

Fields LibraryFields(const SoundLibrary& l)
{
    Fields f = {{"uid", Enc(l.uid)}, {"name", Enc(l.name)}, {"root", Enc(l.root)}};
    if (l.color != 0) f.push_back({"color", FmtColor(l.color)});
    if (l.user) f.push_back({"user", "1"});
    if (!l.ravlib_path.empty()) f.push_back({"ravlib", Enc(l.ravlib_path)});
    return f;
}

Got SetLibrary(SoundLibrary& l, const std::string& k, const std::string& v)
{
    if (k == "uid") return G(ReadStr(v, &l.uid));
    if (k == "name") return G(ReadStr(v, &l.name));
    if (k == "root") return G(ReadStr(v, &l.root));
    if (k == "color") return G(ReadColor(v, &l.color));
    if (k == "user") return G(ReadFlag(v, &l.user));
    if (k == "ravlib") return G(ReadStr(v, &l.ravlib_path));
    return Got::Unknown;
}

Fields NodeFields(const SoundNode& n)
{
    Fields f = {{"uid", Enc(n.uid)}, {"type", kNodeWords[static_cast<int>(n.type)]}};
    if (!n.parent.empty()) f.push_back({"parent", Enc(n.parent)});
    if (!n.name.empty()) f.push_back({"name", Enc(n.name)});
    if (n.volume_db != 0.0) f.push_back({"vol", FormatNumber(n.volume_db)});
    if (n.offset_ms != 0.0) f.push_back({"offset_ms", FormatNumber(n.offset_ms)});
    if (n.avoid_repeat != 2) f.push_back({"avoid", FmtInt(n.avoid_repeat)});
    if (!n.group_uid.empty()) f.push_back({"group", Enc(n.group_uid)});
    if (!n.default_value.empty()) f.push_back({"default", Enc(n.default_value)});
    if (!n.preview_value.empty()) f.push_back({"preview", Enc(n.preview_value)});
    if (!n.lands_in.empty()) f.push_back({"lands", Enc(n.lands_in)});
    return f;
}

Got SetNode(SoundNode& n, const std::string& k, const std::string& v)
{
    if (k == "uid") return G(ReadStr(v, &n.uid));
    if (k == "type") return G(SoundNodeTypeFromWord(v, &n.type));
    if (k == "parent") return G(ReadStr(v, &n.parent));
    if (k == "name") return G(ReadStr(v, &n.name));
    if (k == "vol") return G(ReadNumber(v, &n.volume_db));
    if (k == "offset_ms") return G(ReadNumber(v, &n.offset_ms));
    if (k == "avoid") return G(ReadInt(v, &n.avoid_repeat));
    if (k == "group") return G(ReadStr(v, &n.group_uid));
    if (k == "default") return G(ReadStr(v, &n.default_value));
    if (k == "preview") return G(ReadStr(v, &n.preview_value));
    if (k == "lands") return G(ReadStr(v, &n.lands_in));
    return Got::Unknown;
}

Fields CaseFields(const SwitchCase& c)
{
    return {{"value", Enc(c.value_uid)}, {"child", Enc(c.child_uid)}};
}

Got SetCase(SwitchCase& c, const std::string& k, const std::string& v)
{
    if (k == "value") return G(ReadStr(v, &c.value_uid));
    if (k == "child") return G(ReadStr(v, &c.child_uid));
    return Got::Unknown;
}

Fields SoundFields(const SoundData& s);

// Any sound data at all (a non-Sound node may hold some, e.g. after a type change): written.
bool HasSoundLine(const SoundNode& n)
{
    const SoundData& s = n.sound;
    return n.type == SoundNodeType::Sound || !s.kept.empty() || !s.frames.empty() ||
           SoundFields(s) != SoundFields(SoundData{});
}

Fields SoundFields(const SoundData& s)
{
    const SoundData      d;
    const AssetIdentity& a = s.source.asset;
    Fields f = {{"kind", s.source.kind == SoundSourceKind::File ? "file" : "item"}};
    if (!s.source.item_guid.empty()) f.push_back({"guid", Enc(s.source.item_guid)});
    if (s.mode != d.mode) f.push_back({"mode", kModeWords[static_cast<int>(s.mode)]});
    if (s.silence.threshold_db != d.silence.threshold_db) f.push_back({"thr", FormatNumber(s.silence.threshold_db)});
    if (s.silence.min_gap_ms != d.silence.min_gap_ms) f.push_back({"gap", FormatNumber(s.silence.min_gap_ms)});
    if (s.silence.preroll_ms != d.silence.preroll_ms) f.push_back({"pre", FormatNumber(s.silence.preroll_ms)});
    if (s.silence.tail_ms != d.silence.tail_ms) f.push_back({"tail", FormatNumber(s.silence.tail_ms)});
    if (s.edited) f.push_back({"edited", "1"});
    if (!s.linked_copies) f.push_back({"linked", "0"});
    if (s.fade_in != d.fade_in) f.push_back({"fin", FormatNumber(s.fade_in)});
    if (s.fade_out != d.fade_out) f.push_back({"fout", FormatNumber(s.fade_out)});
    if (!a.name.empty()) f.push_back({"aname", Enc(a.name)});
    if (a.size != -1) f.push_back({"asize", FmtInt(a.size)});
    if (a.length != -1.0) f.push_back({"alen", FormatNumber(a.length)});
    if (!a.last_path.empty()) f.push_back({"apath", Enc(a.last_path)});
    if (!a.rel_path.empty()) f.push_back({"arel", Enc(a.rel_path)});
    if (a.mtime != 0) f.push_back({"amtime", FmtInt(a.mtime)});
    return f;
}

Got SetSound(SoundData& s, const std::string& k, const std::string& v)
{
    AssetIdentity& a = s.source.asset;
    if (k == "kind") {
        if (v == "item") s.source.kind = SoundSourceKind::Item;
        else if (v == "file") s.source.kind = SoundSourceKind::File;
        else return Got::Bad;
        return Got::Ok;
    }
    if (k == "guid") return G(ReadStr(v, &s.source.item_guid));
    if (k == "mode") return G(ModeFromWord(v, &s.mode));
    if (k == "thr") return G(ReadNumber(v, &s.silence.threshold_db));
    if (k == "gap") return G(ReadNumber(v, &s.silence.min_gap_ms));
    if (k == "pre") return G(ReadNumber(v, &s.silence.preroll_ms));
    if (k == "tail") return G(ReadNumber(v, &s.silence.tail_ms));
    if (k == "edited") return G(ReadFlag(v, &s.edited));
    if (k == "linked") return G(ReadFlag(v, &s.linked_copies));
    if (k == "fin") return G(ReadNumber(v, &s.fade_in));
    if (k == "fout") return G(ReadNumber(v, &s.fade_out));
    if (k == "aname") return G(ReadStr(v, &a.name));
    if (k == "asize") return G(ReadInt(v, &a.size));
    if (k == "alen") return G(ReadNumber(v, &a.length));
    if (k == "apath") return G(ReadStr(v, &a.last_path));
    if (k == "arel") return G(ReadStr(v, &a.rel_path));
    if (k == "amtime") return G(ReadInt(v, &a.mtime));
    return Got::Unknown;
}

Fields FrameFields(const SoundFrame& fr)
{
    Fields f = {{"start", FormatNumber(fr.start)}, {"len", FormatNumber(fr.length)}};
    if (!fr.on) f.push_back({"on", "0"});
    if (fr.db != 0.0) f.push_back({"db", FormatNumber(fr.db)});
    if (fr.hand) f.push_back({"hand", "1"});
    if (fr.fade_in >= 0.0) f.push_back({"fin", FormatNumber(fr.fade_in)});
    if (fr.fade_out >= 0.0) f.push_back({"fout", FormatNumber(fr.fade_out)});
    if (fr.fade_in_shape != 0) f.push_back({"fish", FmtInt(fr.fade_in_shape)});
    if (fr.fade_out_shape != 0) f.push_back({"fosh", FmtInt(fr.fade_out_shape)});
    return f;
}

Got SetFrame(SoundFrame& fr, const std::string& k, const std::string& v)
{
    if (k == "start") return G(ReadNumber(v, &fr.start));
    if (k == "len") return G(ReadNumber(v, &fr.length));
    if (k == "on") return G(ReadFlag(v, &fr.on));
    if (k == "db") return G(ReadNumber(v, &fr.db));
    if (k == "hand") return G(ReadFlag(v, &fr.hand));
    if (k == "fin") return G(ReadNumber(v, &fr.fade_in));
    if (k == "fout") return G(ReadNumber(v, &fr.fade_out));
    if (k == "fish") return G(ReadInt(v, &fr.fade_in_shape));
    if (k == "fosh") return G(ReadInt(v, &fr.fade_out_shape));
    return Got::Unknown;
}

// ---- Item record: fields ----------------------------------------------------------------------

Fields ValueSetFields(const ValueSet& x)
{
    return {{"group", Enc(x.group_uid)}, {"value", Enc(x.value_uid)}};
}

Got SetValueSet(ValueSet& x, const std::string& k, const std::string& v)
{
    if (k == "group") return G(ReadStr(v, &x.group_uid));
    if (k == "value") return G(ReadStr(v, &x.value_uid));
    return Got::Unknown;
}

Fields RuleFields(const RuleSounds& r)
{
    Fields f = {{"rid", Enc(r.rid)}};
    if (!r.library.empty()) f.push_back({"lib", Enc(r.library)});
    return f;
}

Got SetRule(RuleSounds& r, const std::string& k, const std::string& v)
{
    if (k == "rid") return G(ReadStr(v, &r.rid));
    if (k == "lib") return G(ReadStr(v, &r.library));
    return Got::Unknown;
}

Fields EventFields(const EventSounds& e)
{
    return {{"rid", Enc(e.rid)}, {"t", FormatNumber(e.t)}};
}

Got SetEvent(EventSounds& e, const std::string& k, const std::string& v)
{
    if (k == "rid") return G(ReadStr(v, &e.rid));
    if (k == "t") return G(ReadNumber(v, &e.t));
    return Got::Unknown;
}

Fields DrawFields(const SoundDraw& d)
{
    Fields f = {{"node", Enc(d.node)}, {"pick", FmtInt(d.pick)}};
    if (d.hand) f.push_back({"hand", "1"});
    return f;
}

Got SetDraw(SoundDraw& d, const std::string& k, const std::string& v)
{
    if (k == "node") return G(ReadStr(v, &d.node));
    if (k == "pick") return G(ReadInt(v, &d.pick));
    if (k == "hand") return G(ReadFlag(v, &d.hand));
    return Got::Unknown;
}

Fields PlaceFields(const SoundPlacement& p)
{
    Fields f = {{"item", Enc(p.item_guid)}, {"node", Enc(p.node)}, {"own", OwnershipWord(p.own)}};
    if (p.offset_s != 0.0) f.push_back({"off", FormatNumber(p.offset_s)});
    if (p.variation != -1) f.push_back({"var", FmtInt(p.variation)});
    if (!p.track_guid.empty()) f.push_back({"track", Enc(p.track_guid)});
    return f;
}

Got SetPlace(SoundPlacement& p, const std::string& k, const std::string& v)
{
    if (k == "item") return G(ReadStr(v, &p.item_guid));
    if (k == "node") return G(ReadStr(v, &p.node));
    if (k == "own") return G(OwnershipFromWord(v, &p.own));
    if (k == "off") return G(ReadNumber(v, &p.offset_s));
    if (k == "var") return G(ReadInt(v, &p.variation));
    if (k == "track") return G(ReadStr(v, &p.track_guid));
    return Got::Unknown;
}

void WriteHeader(std::string& out, const char* magic, int version, const Fields& f, const Kept& kept)
{
    std::string kind = std::string(magic) + ' ' + FmtInt(version < 1 ? 1 : version);
    WriteLine(out, kind.c_str(), f, kept);
}

// ---- Parsers ----------------------------------------------------------------------------------

bool ParseStoreCore(const std::string& text, SoundStore& s)
{
    const std::vector<std::string> lines = SplitLines(text);
    size_t                         i = 0;
    std::vector<std::string>       rest;
    if (!ReadHeader(lines, kSoundStoreMagic, &i, &s.version, &rest)) return false;
    ReadFields(rest, 0, s.kept, [](const std::string&, const std::string&) { return Got::Unknown; });
    Kept*         unknown_to = &s.kept_lines;
    SwitchGroup*  group = nullptr;
    SoundLibrary* lib = nullptr;
    SoundNode*    node = nullptr;
    for (; i < lines.size(); ++i) {
        std::vector<std::string> t;
        const std::string        kind = KindOf(lines[i], &t);
        if (kind == "group") {
            s.groups.emplace_back();
            group = &s.groups.back();
            lib = nullptr;  // a later case/frame/sound line never attaches to an earlier library's node
            node = nullptr;
            ReadFields(t, 1, group->kept, [&](const std::string& k, const std::string& v) { return SetGroup(*group, k, v); });
            unknown_to = &group->kept_lines;
        } else if (kind == "value" && group) {
            group->values.emplace_back();
            SwitchValue& x = group->values.back();
            ReadFields(t, 1, x.kept, [&](const std::string& k, const std::string& v) { return SetValue(x, k, v); });
        } else if (kind == "library") {
            s.libraries.emplace_back();
            lib = &s.libraries.back();
            node = nullptr;
            group = nullptr;
            ReadFields(t, 1, lib->kept, [&](const std::string& k, const std::string& v) { return SetLibrary(*lib, k, v); });
            unknown_to = &lib->kept_lines;
        } else if (kind == "node" && lib) {
            lib->nodes.emplace_back();
            node = &lib->nodes.back();
            ReadFields(t, 1, node->kept, [&](const std::string& k, const std::string& v) { return SetNode(*node, k, v); });
            unknown_to = &node->kept_lines;
        } else if (kind == "case" && node) {
            node->cases.emplace_back();
            SwitchCase& c = node->cases.back();
            ReadFields(t, 1, c.kept, [&](const std::string& k, const std::string& v) { return SetCase(c, k, v); });
        } else if (kind == "unassigned" && node && t.size() == 2 && t[1].compare(0, 6, "child=") == 0) {
            std::string id;
            if (!Dec(t[1].substr(6), &id)) unknown_to->push_back(lines[i]);
            else node->unassigned.push_back(id);
        } else if (kind == "sound" && node) {
            ReadFields(t, 1, node->sound.kept,
                       [&](const std::string& k, const std::string& v) { return SetSound(node->sound, k, v); });
        } else if (kind == "frame" && node) {
            node->sound.frames.emplace_back();
            SoundFrame& fr = node->sound.frames.back();
            ReadFields(t, 1, fr.kept, [&](const std::string& k, const std::string& v) { return SetFrame(fr, k, v); });
        } else {
            unknown_to->push_back(lines[i]);
        }
    }
    return true;
}

bool ParseItemCore(const std::string& text, ItemSounds& s)
{
    const std::vector<std::string> lines = SplitLines(text);
    size_t                         i = 0;
    std::vector<std::string>       rest;
    if (!ReadHeader(lines, kItemSoundsMagic, &i, &s.version, &rest)) return false;
    ReadFields(rest, 0, s.kept, [&](const std::string& k, const std::string& v) {
        if (k == "seed") return G(ReadInt(v, &s.seed));
        return Got::Unknown;
    });
    Kept*        unknown_to = &s.kept_lines;
    RuleSounds*  rule = nullptr;
    EventSounds* ev = nullptr;
    for (; i < lines.size(); ++i) {
        std::vector<std::string> t;
        const std::string        kind = KindOf(lines[i], &t);
        if (kind == "ivalue" && !rule && !ev) {
            s.item_values.emplace_back();
            ValueSet& x = s.item_values.back();
            ReadFields(t, 1, x.kept, [&](const std::string& k, const std::string& v) { return SetValueSet(x, k, v); });
        } else if (kind == "rule") {
            s.rules.emplace_back();
            rule = &s.rules.back();
            ev = nullptr;
            ReadFields(t, 1, rule->kept, [&](const std::string& k, const std::string& v) { return SetRule(*rule, k, v); });
            unknown_to = &rule->kept_lines;
        } else if (kind == "rvalue" && rule && !ev) {
            rule->values.emplace_back();
            ValueSet& x = rule->values.back();
            ReadFields(t, 1, x.kept, [&](const std::string& k, const std::string& v) { return SetValueSet(x, k, v); });
        } else if (kind == "event") {
            s.events.emplace_back();
            ev = &s.events.back();
            rule = nullptr;
            ReadFields(t, 1, ev->kept, [&](const std::string& k, const std::string& v) { return SetEvent(*ev, k, v); });
            unknown_to = &ev->kept_lines;
        } else if (kind == "evalue" && ev) {
            ev->overrides.emplace_back();
            ValueSet& x = ev->overrides.back();
            ReadFields(t, 1, x.kept, [&](const std::string& k, const std::string& v) { return SetValueSet(x, k, v); });
        } else if (kind == "draw" && ev) {
            ev->draws.emplace_back();
            SoundDraw& d = ev->draws.back();
            ReadFields(t, 1, d.kept, [&](const std::string& k, const std::string& v) { return SetDraw(d, k, v); });
        } else if (kind == "place" && ev) {
            ev->placed.emplace_back();
            SoundPlacement& p = ev->placed.back();
            ReadFields(t, 1, p.kept, [&](const std::string& k, const std::string& v) { return SetPlace(p, k, v); });
        } else {
            unknown_to->push_back(lines[i]);
        }
    }
    return true;
}

}  // namespace

// ---- Words ------------------------------------------------------------------------------------

const char* SoundNodeTypeWord(SoundNodeType t)
{
    const int i = static_cast<int>(t);
    return i >= 0 && i < 4 ? kNodeWords[i] : "random";
}

bool SoundNodeTypeFromWord(const std::string& s, SoundNodeType* out)
{
    for (int i = 0; i < 4; ++i)
        if (s == kNodeWords[i]) {
            *out = static_cast<SoundNodeType>(i);
            return true;
        }
    return false;
}

const char* OwnershipWord(Ownership o)
{
    const int i = static_cast<int>(o);
    return i >= 0 && i < 4 ? kOwnWords[i] : "populated";
}

bool OwnershipFromWord(const std::string& s, Ownership* out)
{
    for (int i = 0; i < 4; ++i)
        if (s == kOwnWords[i]) {
            *out = static_cast<Ownership>(i);
            return true;
        }
    return false;
}

// ---- Store ------------------------------------------------------------------------------------

bool ParseSoundStore(const std::string& text, SoundStore* out)
{
    if (!out) return false;
    try {
        SoundStore s;
        if (!ParseStoreCore(text, s)) return false;
        *out = std::move(s);
        return true;
    } catch (...) {
        return false;
    }
}

bool SoundStoreEmpty(const SoundStore& s)
{
    return s.groups.empty() && s.libraries.empty() && s.kept.empty() && s.kept_lines.empty();
}

std::string SerializeSoundStore(const SoundStore& s)
{
    if (SoundStoreEmpty(s)) return std::string();
    std::string out;
    WriteHeader(out, kSoundStoreMagic, s.version, {}, s.kept);
    WriteLines(out, s.kept_lines);
    for (const SwitchGroup& g : s.groups) {
        WriteLine(out, "group", GroupFields(g), g.kept);
        for (const SwitchValue& v : g.values) WriteLine(out, "value", ValueFields(v), v.kept);
        WriteLines(out, g.kept_lines);
    }
    for (const SoundLibrary& l : s.libraries) {
        WriteLine(out, "library", LibraryFields(l), l.kept);
        WriteLines(out, l.kept_lines);
        for (const SoundNode& n : l.nodes) {
            WriteLine(out, "node", NodeFields(n), n.kept);
            for (const SwitchCase& c : n.cases) WriteLine(out, "case", CaseFields(c), c.kept);
            for (const std::string& u : n.unassigned) WriteLine(out, "unassigned", {{"child", Enc(u)}}, {});
            if (HasSoundLine(n)) {
                WriteLine(out, "sound", SoundFields(n.sound), n.sound.kept);
                for (const SoundFrame& fr : n.sound.frames) WriteLine(out, "frame", FrameFields(fr), fr.kept);
            }
            WriteLines(out, n.kept_lines);
        }
    }
    return out;
}

// ---- Item record ------------------------------------------------------------------------------

bool ParseItemSounds(const std::string& text, ItemSounds* out)
{
    if (!out) return false;
    try {
        ItemSounds s;
        if (!ParseItemCore(text, s)) return false;
        *out = std::move(s);
        return true;
    } catch (...) {
        return false;
    }
}

bool ItemSoundsEmpty(const ItemSounds& s)
{
    return s.seed == 0 && s.item_values.empty() && s.rules.empty() && s.events.empty() && s.kept.empty() &&
           s.kept_lines.empty();
}

std::string SerializeItemSounds(const ItemSounds& s)
{
    std::string out;
    Fields      head;
    if (s.seed != 0) head.push_back({"seed", FmtInt(s.seed)});
    WriteHeader(out, kItemSoundsMagic, s.version, head, s.kept);
    WriteLines(out, s.kept_lines);
    for (const ValueSet& v : s.item_values) WriteLine(out, "ivalue", ValueSetFields(v), v.kept);
    for (const RuleSounds& r : s.rules) {
        WriteLine(out, "rule", RuleFields(r), r.kept);
        for (const ValueSet& v : r.values) WriteLine(out, "rvalue", ValueSetFields(v), v.kept);
        WriteLines(out, r.kept_lines);
    }
    for (const EventSounds& e : s.events) {
        WriteLine(out, "event", EventFields(e), e.kept);
        for (const ValueSet& v : e.overrides) WriteLine(out, "evalue", ValueSetFields(v), v.kept);
        for (const SoundDraw& d : e.draws) WriteLine(out, "draw", DrawFields(d), d.kept);
        for (const SoundPlacement& p : e.placed) WriteLine(out, "place", PlaceFields(p), p.kept);
        WriteLines(out, e.kept_lines);
    }
    return out;
}

// ---- Placed tag -------------------------------------------------------------------------------

bool ParsePlacedTag(const std::string& text, PlacedTag* out)
{
    if (!out) return false;
    try {
        const std::vector<std::string> lines = SplitLines(text);
        size_t                         i = 0;
        int                            version = 0;
        std::vector<std::string>       rest;
        if (!ReadHeader(lines, kPlacedTagMagic, &i, &version, &rest)) return false;
        PlacedTag t;
        t.version = version;
        bool      owner = false, ao = false, sy = false;
        ReadFields(rest, 0, t.kept, [&](const std::string& k, const std::string& v) {
            if (k == "owner") return (owner = ReadStr(v, &t.owner)) ? Got::Ok : Got::Bad;
            if (k == "rid") return G(ReadStr(v, &t.rid));
            if (k == "t") return G(ReadNumber(v, &t.t));
            if (k == "node") return G(ReadStr(v, &t.node));
            if (k == "own") return G(OwnershipFromWord(v, &t.own));
            if (k == "ao") return G(ao = ReadNumber(v, &t.ao));
            if (k == "sy") return G(sy = ReadNumber(v, &t.sy));
            return Got::Unknown;
        });
        if (!owner || t.owner.empty()) return false;
        t.has_anchor = ao && sy;
        *out = std::move(t);
        return true;
    } catch (...) {
        return false;
    }
}

std::string SerializePlacedTag(const PlacedTag& t)
{
    std::string out;
    Fields      f = {{"owner", Enc(t.owner)},
                     {"rid", Enc(t.rid)},
                     {"t", FormatNumber(t.t)},
                     {"node", Enc(t.node)},
                     {"own", OwnershipWord(t.own)}};
    if (t.has_anchor) {
        f.push_back({"ao", FormatNumber(t.ao)});
        f.push_back({"sy", FormatNumber(t.sy)});
    }
    WriteHeader(out, kPlacedTagMagic, t.version, f, t.kept);
    return out;
}

// ---- Lookups ----------------------------------------------------------------------------------

const SoundLibrary* FindLibrary(const SoundStore& store, const std::string& uid)
{
    for (const SoundLibrary& l : store.libraries)
        if (l.uid == uid) return &l;
    return nullptr;
}

SoundLibrary* FindLibrary(SoundStore& store, const std::string& uid)
{
    return const_cast<SoundLibrary*>(FindLibrary(static_cast<const SoundStore&>(store), uid));
}

const SoundNode* FindNode(const SoundLibrary& lib, const std::string& uid)
{
    if (uid.empty()) return nullptr;
    for (const SoundNode& n : lib.nodes)
        if (n.uid == uid) return &n;
    return nullptr;
}

SoundNode* FindNode(SoundLibrary& lib, const std::string& uid)
{
    return const_cast<SoundNode*>(FindNode(static_cast<const SoundLibrary&>(lib), uid));
}

std::vector<const SoundNode*> ChildrenOf(const SoundLibrary& lib, const std::string& uid)
{
    std::vector<const SoundNode*> out;
    if (uid.empty()) {
        if (const SoundNode* r = FindNode(lib, lib.root)) out.push_back(r);
        return out;
    }
    for (const SoundNode& n : lib.nodes)
        if (n.parent == uid && n.uid != uid) out.push_back(&n);
    return out;
}

const SwitchGroup* FindGroup(const SoundStore& store, const std::string& uid)
{
    for (const SwitchGroup& g : store.groups)
        if (g.uid == uid) return &g;
    return nullptr;
}

SwitchGroup* FindGroup(SoundStore& store, const std::string& uid)
{
    return const_cast<SwitchGroup*>(FindGroup(static_cast<const SoundStore&>(store), uid));
}

const EventSounds* FindEventSounds(const ItemSounds& item, const std::string& rid, double t, double window)
{
    const EventSounds* best = nullptr;
    double             best_d = 0.0;
    for (const EventSounds& e : item.events) {
        if (e.rid != rid) continue;
        const double d = std::fabs(e.t - t);
        if (d > window + 1e-9) continue;
        if (!best || d < best_d) {
            best = &e;
            best_d = d;
        }
    }
    return best;
}

EventSounds* FindEventSounds(ItemSounds& item, const std::string& rid, double t, double window)
{
    return const_cast<EventSounds*>(FindEventSounds(static_cast<const ItemSounds&>(item), rid, t, window));
}

const RuleSounds* FindRuleSounds(const ItemSounds& item, const std::string& rid)
{
    for (const RuleSounds& r : item.rules)
        if (r.rid == rid) return &r;
    return nullptr;
}

RuleSounds* FindRuleSounds(ItemSounds& item, const std::string& rid)
{
    return const_cast<RuleSounds*>(FindRuleSounds(static_cast<const ItemSounds&>(item), rid));
}

}  // namespace rav
