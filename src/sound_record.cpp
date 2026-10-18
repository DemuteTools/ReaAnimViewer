// SPDX-License-Identifier: MIT
//
// See sound_record.h.

#include "sound_record.h"

#include "normalize_presets.h"  // kNormModeCount, kNormTargetMin / Max (13-62 review)
#include "sound_zones.h"       // MigrateLegacyZones (13-62h)

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <algorithm>
#include <utility>
#include <vector>

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
const char* const kSyncWords[] = {"attack", "peak", "start"};
// 13-41: MonoMode, Off never written.
const char* const kMonoWords[] = {"off", "left", "right", "sum"};
const char* const kSidePatternWords[] = {"lr", "rl", "left", "right"};  // 13-73: SidePattern

bool MonoFromWord(const std::string& s, MonoMode* out)
{
    for (int i = 0; i < 4; ++i)
        if (s == kMonoWords[i]) {
            *out = static_cast<MonoMode>(i);
            return true;
        }
    return false;
}

bool SyncFromWord(const std::string& s, SyncMode* out)
{
    for (int i = 0; i < 3; ++i)
        if (s == kSyncWords[i]) {
            *out = static_cast<SyncMode>(i);
            return true;
        }
    return false;
}

bool ModeFromWord(const std::string& s, DetectMode* out)
{
    for (int i = 0; i < 4; ++i)
        if (s == kModeWords[i]) {
            *out = static_cast<DetectMode>(i);
            return true;
        }
    return false;
}

// ---- Lists ------------------------------------------------------------------------------------

std::vector<std::string> SplitCommas(const std::string& s)
{
    std::vector<std::string> out;
    size_t                   pos = 0;
    while (pos <= s.size()) {
        size_t c = s.find(',', pos);
        if (c == std::string::npos) c = s.size();
        if (c > pos) out.push_back(s.substr(pos, c - pos));
        pos = c + 1;
    }
    return out;
}

// Names joined by ',' (each percent-encoded, so a ',' in a name cannot split it).
std::string JoinEncoded(const std::vector<std::string>& names)
{
    std::string list;
    for (const std::string& n : names) list += (list.empty() ? "" : ",") + Enc(n);
    return list;
}

bool SplitDecoded(const std::string& v, std::vector<std::string>* out)
{
    std::vector<std::string> names;
    for (const std::string& part : SplitCommas(v)) {
        std::string n;
        if (!ReadStr(part, &n)) return false;
        names.push_back(n);
    }
    *out = names;
    return true;
}

// 13-23: numbers joined by ',' (C locale); empty = none.
std::string JoinNumbers(const std::vector<double>& v)
{
    std::string list;
    for (double x : v) list += (list.empty() ? "" : ",") + FormatNumber(x);
    return list;
}

bool SplitNumbers(const std::string& v, std::vector<double>* out)
{
    std::vector<double> nums;
    for (const std::string& part : SplitCommas(v)) {
        double x = 0.0;
        if (!ReadNumber(part, &x)) return false;
        nums.push_back(x);
    }
    *out = nums;
    return true;
}

// 13-23: a Sound's own role map, "heel:<uid>,toe_end:<uid>" (each side percent-encoded; split on the
// last ':', a uid never holds one).
std::string JoinRoleMap(const std::vector<RoleValue>& m)
{
    std::string list;
    for (const RoleValue& r : m) list += (list.empty() ? "" : ",") + Enc(r.role) + ":" + Enc(r.value);
    return list;
}

bool SplitRoleMap(const std::string& v, std::vector<RoleValue>* out)
{
    std::vector<RoleValue> m;
    for (const std::string& part : SplitCommas(v)) {
        const size_t colon = part.rfind(':');
        if (colon == std::string::npos) return false;
        RoleValue r;
        if (!ReadStr(part.substr(0, colon), &r.role) || !ReadStr(part.substr(colon + 1), &r.value)) return false;
        m.push_back(r);
    }
    *out = m;
    return true;
}

// ---- Store: fields ----------------------------------------------------------------------------

Fields GroupFields(const SwitchGroup& g)
{
    Fields f = {{"uid", Enc(g.uid)}, {"name", Enc(g.name)}};
    if (!g.default_value.empty()) f.push_back({"default", Enc(g.default_value)});
    if (g.rav_owned) f.push_back({"rav", "1"});  // 13-68 v3
    return f;
}

Got SetGroup(SwitchGroup& g, const std::string& k, const std::string& v)
{
    if (k == "uid") return G(ReadStr(v, &g.uid));
    if (k == "name") return G(ReadStr(v, &g.name));
    if (k == "default") return G(ReadStr(v, &g.default_value));
    if (k == "rav") return G(ReadFlag(v, &g.rav_owned));
    return Got::Unknown;
}

Fields ValueFields(const SwitchValue& x)
{
    Fields f = {{"uid", Enc(x.uid)}, {"name", Enc(x.name)}};
    if (!x.roles.empty()) f.push_back({"roles", JoinEncoded(x.roles)});
    return f;
}

Got SetValue(SwitchValue& x, const std::string& k, const std::string& v)
{
    if (k == "uid") return G(ReadStr(v, &x.uid));
    if (k == "name") return G(ReadStr(v, &x.name));
    if (k == "roles") return G(SplitDecoded(v, &x.roles));
    return Got::Unknown;
}

Fields LibraryFields(const SoundLibrary& l)
{
    Fields f = {{"uid", Enc(l.uid)}, {"name", Enc(l.name)}, {"root", Enc(l.root)}};
    if (l.color != 0) f.push_back({"color", FmtColor(l.color)});
    if (l.user) f.push_back({"user", "1"});
    if (l.attached) f.push_back({"attached", "1"});  // 13-68 v2
    if (!l.attached_type.empty()) f.push_back({"atype", Enc(l.attached_type)});
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
    if (k == "attached") return G(ReadFlag(v, &l.attached));
    if (k == "atype") return G(ReadStr(v, &l.attached_type));
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
    if (!n.layer_roles.empty()) f.push_back({"roles", JoinEncoded(n.layer_roles)});  // 13-31
    if (n.layer_keep_offset) f.push_back({"koff", "1"});
    if (!n.was_path.empty()) f.push_back({"wpath", Enc(n.was_path)});  // 13-72
    if (!n.was_switch.empty()) f.push_back({"wsw", Enc(n.was_switch)});
    if (!n.was_value.empty()) f.push_back({"wval", Enc(n.was_value)});
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
    if (k == "roles") return G(SplitDecoded(v, &n.layer_roles));
    if (k == "koff") return G(ReadFlag(v, &n.layer_keep_offset));
    if (k == "lvar") return Got::Ok;  // 13-68 v2's linked layers flag, read and dropped (v3: every Blend links)
    if (k == "wpath") return G(ReadStr(v, &n.was_path));
    if (k == "wsw") return G(ReadStr(v, &n.was_switch));
    if (k == "wval") return G(ReadStr(v, &n.was_value));
    if (k == "part") return Got::Ok;  // 13-31: the retired 13-23 layer part, read and dropped
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
    return n.type == SoundNodeType::Sound || !s.kept.empty() || !s.frames.empty() || !s.zones.empty() || !s.parts.empty() ||
           !s.parked.empty() ||
           SoundFields(s) != SoundFields(SoundData{});
}

// 13-67b: what a `sound` line leaves out reads as these (the defaults before 13-67b: -45 dB, Attack), so the Sounds
// saved before keep their threshold and snap offset mode while a new Sound starts at -60 dB / the Options' mode.
constexpr double   kRecordThresholdDb = -45.0;
constexpr SyncMode kRecordSyncMode = SyncMode::Attack;

void SetRecordBaseline(SoundData& s)
{
    s.silence.threshold_db = kRecordThresholdDb;
    s.sync_mode = kRecordSyncMode;
    s.sides = false;  // 13-73: a Sound saved without `sides` has Left / Right off (whatever the New sounds pref)
}

Fields SoundFields(const SoundData& s)
{
    SoundData d;
    SetRecordBaseline(d);
    const AssetIdentity& a = s.source.asset;
    Fields f = {{"kind", s.source.kind == SoundSourceKind::File ? "file" : "item"}};
    if (!s.source.item_guid.empty()) f.push_back({"guid", Enc(s.source.item_guid)});
    if (s.source.item_db != 0.0) f.push_back({"idb", FormatNumber(s.source.item_db)});
    if (s.mode != d.mode) f.push_back({"mode", kModeWords[static_cast<int>(s.mode)]});
    if (s.silence.threshold_db != d.silence.threshold_db) f.push_back({"thr", FormatNumber(s.silence.threshold_db)});
    if (s.silence.min_gap_ms != d.silence.min_gap_ms) f.push_back({"gap", FormatNumber(s.silence.min_gap_ms)});
    if (s.silence.preroll_ms != d.silence.preroll_ms) f.push_back({"pre", FormatNumber(s.silence.preroll_ms)});
    if (s.silence.tail_ms != d.silence.tail_ms) f.push_back({"tail", FormatNumber(s.silence.tail_ms)});
    if (s.edited) f.push_back({"edited", "1"});
    if (!s.linked_copies) f.push_back({"linked", "0"});
    if (s.keep_rate) f.push_back({"krate", "1"});  // 13-68 v3
    if (s.fade_in != d.fade_in) f.push_back({"fin", FormatNumber(s.fade_in)});
    if (s.fade_out != d.fade_out) f.push_back({"fout", FormatNumber(s.fade_out)});
    if (!a.name.empty()) f.push_back({"aname", Enc(a.name)});
    if (a.size != -1) f.push_back({"asize", FmtInt(a.size)});
    if (a.length != -1.0) f.push_back({"alen", FormatNumber(a.length)});
    if (!a.last_path.empty()) f.push_back({"apath", Enc(a.last_path)});
    if (!a.rel_path.empty()) f.push_back({"arel", Enc(a.rel_path)});
    if (a.mtime != 0) f.push_back({"amtime", FmtInt(a.mtime)});
    if (!s.divide_group.empty()) f.push_back({"div", Enc(s.divide_group)});
    if (!s.divide_values.empty()) f.push_back({"divv", JoinEncoded(s.divide_values)});
    if (!s.role_map.empty()) f.push_back({"rmap", JoinRoleMap(s.role_map)});
    if (s.lanes_split) f.push_back({"lanes", "split"});
    if (s.sync_mode != d.sync_mode) f.push_back({"sync", kSyncWords[static_cast<int>(s.sync_mode)]});
    if (s.sync_offset_ms != d.sync_offset_ms) f.push_back({"soff", FormatNumber(s.sync_offset_ms)});
    if (s.cut_fade != d.cut_fade) f.push_back({"cfade", FormatNumber(s.cut_fade)});
    if (s.gap_xfade != d.gap_xfade) f.push_back({"gxfade", FormatNumber(s.gap_xfade)});  // 13-62h
    if (s.mono != d.mono) f.push_back({"mono", kMonoWords[static_cast<int>(s.mono)]});
    if (s.fader_db) f.push_back({"fdr", FormatNumber(*s.fader_db)});  // 13-62i: the implicit part's fader
    if (s.sides) f.push_back({"sides", "1"});                                  // 13-73
    if (s.side_pattern != d.side_pattern) f.push_back({"spat", kSidePatternWords[static_cast<int>(s.side_pattern)]});
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
    if (k == "idb") return G(ReadNumber(v, &s.source.item_db));
    if (k == "mode") return G(ModeFromWord(v, &s.mode));
    if (k == "thr") return G(ReadNumber(v, &s.silence.threshold_db));
    if (k == "gap") return G(ReadNumber(v, &s.silence.min_gap_ms));
    if (k == "pre") return G(ReadNumber(v, &s.silence.preroll_ms));
    if (k == "tail") return G(ReadNumber(v, &s.silence.tail_ms));
    if (k == "edited") return G(ReadFlag(v, &s.edited));
    if (k == "linked") return G(ReadFlag(v, &s.linked_copies));
    if (k == "krate") return G(ReadFlag(v, &s.keep_rate));
    if (k == "fin") return G(ReadNumber(v, &s.fade_in));
    if (k == "fout") return G(ReadNumber(v, &s.fade_out));
    if (k == "aname") return G(ReadStr(v, &a.name));
    if (k == "asize") return G(ReadInt(v, &a.size));
    if (k == "alen") return G(ReadNumber(v, &a.length));
    if (k == "apath") return G(ReadStr(v, &a.last_path));
    if (k == "arel") return G(ReadStr(v, &a.rel_path));
    if (k == "amtime") return G(ReadInt(v, &a.mtime));
    if (k == "div") return G(ReadStr(v, &s.divide_group));
    if (k == "divv") return G(SplitDecoded(v, &s.divide_values));
    if (k == "rmap") return G(SplitRoleMap(v, &s.role_map));
    if (k == "lanes") {
        if (v == "split") s.lanes_split = true;
        else if (v == "combined") s.lanes_split = false;
        else return Got::Bad;
        return Got::Ok;
    }
    if (k == "sync") return G(SyncFromWord(v, &s.sync_mode));
    if (k == "soff") return G(ReadNumber(v, &s.sync_offset_ms));
    if (k == "cfade") return G(ReadNumber(v, &s.cut_fade));
    if (k == "gxfade") {  // 13-62h: as the At Gaps row allows (0..200 ms)
        double x = 0.0;
        if (!ReadNumber(v, &x) || x < 0.0 || x > 0.2) return Got::Bad;
        s.gap_xfade = x;
        return Got::Ok;
    }
    if (k == "mono") return G(MonoFromWord(v, &s.mono));
    if (k == "fdr") {  // 13-62i
        double x = 0.0;
        if (!ReadNumber(v, &x)) return Got::Bad;
        s.fader_db = x;
        return Got::Ok;
    }
    if (k == "sides") return G(ReadFlag(v, &s.sides));  // 13-73
    if (k == "spat") {
        for (int i = 0; i < 4; ++i)
            if (v == kSidePatternWords[i]) {
                s.side_pattern = static_cast<SidePattern>(i);
                return Got::Ok;
            }
        return Got::Bad;
    }
    return Got::Unknown;
}

// 13-20: a zone of the Sound above (`to` "" = excluded).
Fields ZoneFields(const SoundZone& z)
{
    Fields f = {{"start", FormatNumber(z.start)}, {"len", FormatNumber(z.length)}};
    if (!z.to.empty()) f.push_back({"to", Enc(z.to)});
    return f;
}

Got SetZone(SoundZone& z, const std::string& k, const std::string& v)
{
    if (k == "start") return G(ReadNumber(v, &z.start));
    if (k == "len") return G(ReadNumber(v, &z.length));
    if (k == "to") return G(ReadStr(v, &z.to));
    return Got::Unknown;
}

// 13-62: a part's own settings (only the fields it owns).
Fields PartFields(const PartSettings& p)
{
    Fields f = {{"v", Enc(p.value)}};
    if (p.preroll_ms) f.push_back({"pre", FormatNumber(*p.preroll_ms)});
    if (p.tail_ms) f.push_back({"tail", FormatNumber(*p.tail_ms)});
    if (p.sync_mode) f.push_back({"sync", kSyncWords[static_cast<int>(*p.sync_mode)]});
    if (p.sync_offset_ms) f.push_back({"soff", FormatNumber(*p.sync_offset_ms)});
    if (p.gain_db) f.push_back({"gain", FormatNumber(*p.gain_db)});
    if (p.norm_mode) f.push_back({"nmode", FmtInt(*p.norm_mode)});
    if (p.norm_target) f.push_back({"ntgt", FormatNumber(*p.norm_target)});
    if (p.mono) f.push_back({"mono", kMonoWords[static_cast<int>(*p.mono)]});
    if (p.fade_in) f.push_back({"fin", FormatNumber(*p.fade_in)});
    if (p.fade_out) f.push_back({"fout", FormatNumber(*p.fade_out)});
    if (p.fader_db) f.push_back({"fdr", FormatNumber(*p.fader_db)});  // 13-62h
    return f;
}

// 13-62 review: the bounds SetPartSetting keeps (margins / fades >= 0, the target in -60..0 dB, a known
// Normalize mode); a non-finite or unreadable number is Bad (kept as written).
Got SetPart(PartSettings& p, const std::string& k, const std::string& v)
{
    auto num = [&](std::optional<double>* out, double lo, double hi) {
        double x = 0.0;
        if (!ReadNumber(v, &x)) return Got::Bad;  // non-finite refused there
        *out = std::min(std::max(x, lo), hi);
        return Got::Ok;
    };
    constexpr double kAny = 1e300;
    if (k == "v") return G(ReadStr(v, &p.value));
    if (k == "pre") return num(&p.preroll_ms, 0.0, kAny);
    if (k == "tail") return num(&p.tail_ms, 0.0, kAny);
    if (k == "sync") {
        SyncMode m = SyncMode::Attack;
        if (!SyncFromWord(v, &m)) return Got::Bad;
        p.sync_mode = m;
        return Got::Ok;
    }
    if (k == "soff") return num(&p.sync_offset_ms, -kAny, kAny);
    if (k == "gain") return num(&p.gain_db, -kAny, kAny);
    if (k == "nmode") {
        int m = 0;
        if (!ReadInt(v, &m) || m < 0 || m >= kNormModeCount) return Got::Bad;
        p.norm_mode = m;
        return Got::Ok;
    }
    if (k == "ntgt") return num(&p.norm_target, kNormTargetMin, kNormTargetMax);
    if (k == "mono") {
        MonoMode m = MonoMode::Off;
        if (!MonoFromWord(v, &m)) return Got::Bad;
        p.mono = m;
        return Got::Ok;
    }
    if (k == "fin") return num(&p.fade_in, 0.0, kAny);
    if (k == "fout") return num(&p.fade_out, 0.0, kAny);
    if (k == "fdr") return num(&p.fader_db, -kAny, kAny);  // 13-62h
    return Got::Unknown;
}

Fields FrameFields(const SoundFrame& fr)
{
    Fields f = {{"start", FormatNumber(fr.start)}, {"len", FormatNumber(fr.length)}};
    if (!fr.on) f.push_back({"on", "0"});
    if (fr.db != 0.0 && fr.zones.empty()) f.push_back({"db", FormatNumber(fr.db)});  // 13-62h: zoned = the zones' gains
    if (fr.hand) f.push_back({"hand", "1"});
    if (fr.zones.empty() && fr.fade_in >= 0.0) f.push_back({"fin", FormatNumber(fr.fade_in)});
    if (fr.zones.empty() && fr.fade_out >= 0.0) f.push_back({"fout", FormatNumber(fr.fade_out)});
    if (fr.fade_in_shape != 0) f.push_back({"fish", FmtInt(fr.fade_in_shape)});
    if (fr.fade_out_shape != 0) f.push_back({"fosh", FmtInt(fr.fade_out_shape)});
    if (fr.side == 'L' || fr.side == 'R') f.push_back({"side", std::string(1, fr.side)});  // 13-73
    if (fr.side_hand == 'L' || fr.side_hand == 'R') f.push_back({"sideh", std::string(1, fr.side_hand)});
    // 13-62h: a frame laid out as zones writes its zones (fzone lines), never the old model's keys.
    const bool zoned = !fr.zones.empty();
    if (zoned) return f;
    if (!fr.cuts.empty()) f.push_back({"cuts", JoinNumbers(fr.cuts)});
    if (fr.cuts_hand) f.push_back({"cutsh", "1"});
    if (fr.cut_unsure) f.push_back({"cutq", "1"});
    if (!fr.syncs.empty()) f.push_back({"sync", JoinNumbers(fr.syncs)});
    if (!fr.hand_syncs.empty()) f.push_back({"hsync", JoinNumbers(fr.hand_syncs)});
    if (!fr.part_trims.empty()) f.push_back({"ptrim", JoinNumbers(fr.part_trims)});  // 13-54
    if (!fr.part_trims.empty() && !fr.part_trim_key.empty()) f.push_back({"ptrimk", Enc(fr.part_trim_key)});
    for (const auto& pd : fr.part_db) f.push_back({"pdb." + Enc(pd.first), FormatNumber(pd.second)});  // 13-28
    if (!fr.owners.empty()) f.push_back({"own", JoinEncoded(fr.owners)});  // 13-62
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
    if (k == "side" || k == "sideh") {  // 13-73
        if (v != "L" && v != "R") return Got::Bad;
        (k == "side" ? fr.side : fr.side_hand) = v[0];
        return Got::Ok;
    }
    if (k == "cuts") return G(SplitNumbers(v, &fr.cuts));
    if (k == "cutsh") return G(ReadFlag(v, &fr.cuts_hand));
    if (k == "cutq") return G(ReadFlag(v, &fr.cut_unsure));
    if (k == "sync") return G(SplitNumbers(v, &fr.syncs));
    if (k == "hsync") return G(SplitNumbers(v, &fr.hand_syncs));
    if (k == "ptrim") return G(SplitNumbers(v, &fr.part_trims));
    if (k == "ptrimk") return G(Dec(v, &fr.part_trim_key));  // 13-54 review: what they were measured with
    if (k == "own") return G(SplitDecoded(v, &fr.owners));    // 13-62: who owns each piece
    if (k.size() > 4 && k.compare(0, 4, "pdb.") == 0) {  // 13-28: a part's gain, pdb.<value uid>
        std::string uid;
        double      db = 0.0;
        if (!Dec(k.substr(4), &uid) || uid.empty() || !ReadNumber(v, &db)) return Got::Bad;
        fr.part_db[uid] = db;
        return Got::Ok;
    }
    return Got::Unknown;
}

// 13-62h: a zone of the variation above (`fzone`, after its `frame` line): only the set fields; `p` absent = a
// Gap; sa / sb only when they differ from a / b.
Fields FzoneFields(const EditZone& z)
{
    Fields f = {{"a", FormatNumber(z.a)}, {"b", FormatNumber(z.b)}};
    if (!z.part.empty()) f.push_back({"p", Enc(z.part)});
    if (z.slot_a != z.a) f.push_back({"sa", FormatNumber(z.slot_a)});
    if (z.slot_b != z.b) f.push_back({"sb", FormatNumber(z.slot_b)});
    if (z.a_hand) f.push_back({"ah", "1"});
    if (z.b_hand) f.push_back({"bh", "1"});
    if (z.a_auto_cut) f.push_back({"acut", "1"});
    if (z.onset >= 0.0) f.push_back({"on", FormatNumber(z.onset)});
    if (z.snap) f.push_back({"snap", FormatNumber(*z.snap)});
    if (z.fade_in) f.push_back({"fin", FormatNumber(*z.fade_in)});
    if (z.fade_out) f.push_back({"fout", FormatNumber(*z.fade_out)});
    if (z.db != 0.0) f.push_back({"db", FormatNumber(z.db)});
    if (z.xl != 0.0) f.push_back({"xl", FormatNumber(z.xl)});
    if (z.xr != 0.0) f.push_back({"xr", FormatNumber(z.xr)});
    if (z.fade_in_shape != 0) f.push_back({"fis", FmtInt(z.fade_in_shape)});  // 13-62i
    if (z.fade_out_shape != 0) f.push_back({"fos", FmtInt(z.fade_out_shape)});
    if (z.fade_in_dir) f.push_back({"fid", FormatNumber(*z.fade_in_dir)});  // 13-75: a curvature not its shape's preset
    if (z.fade_out_dir) f.push_back({"fod", FormatNumber(*z.fade_out_dir)});
    if (z.auto_snap.set) {  // 13-62h review (R2): its measured automatic snap and what it was measured for
        std::vector<double> n = {z.auto_snap.t, z.auto_snap.a, z.auto_snap.b, static_cast<double>(z.auto_snap.mode), z.auto_snap.thr,
                                 z.auto_snap.off};
        if (z.auto_snap.fine) n.push_back(1.0);  // fix 2026-10-19: measured on the samples
        f.push_back({"as", JoinNumbers(n)});
    }
    return f;
}

Got SetFzone(EditZone& z, bool* has_sa, bool* has_sb, const std::string& k, const std::string& v)
{
    auto opt = [&](std::optional<double>* out) {
        double x = 0.0;
        if (!ReadNumber(v, &x)) return Got::Bad;
        *out = x;
        return Got::Ok;
    };
    if (k == "a") return G(ReadNumber(v, &z.a));
    if (k == "b") return G(ReadNumber(v, &z.b));
    if (k == "p") return G(ReadStr(v, &z.part));
    if (k == "sa") return (*has_sa = ReadNumber(v, &z.slot_a)) ? Got::Ok : Got::Bad;
    if (k == "sb") return (*has_sb = ReadNumber(v, &z.slot_b)) ? Got::Ok : Got::Bad;
    if (k == "ah") return G(ReadFlag(v, &z.a_hand));
    if (k == "bh") return G(ReadFlag(v, &z.b_hand));
    if (k == "acut") return G(ReadFlag(v, &z.a_auto_cut));
    if (k == "on") return G(ReadNumber(v, &z.onset));
    if (k == "snap") return opt(&z.snap);
    if (k == "fin") return opt(&z.fade_in);
    if (k == "fout") return opt(&z.fade_out);
    if (k == "db") return G(ReadNumber(v, &z.db));
    if (k == "xl") return G(ReadNumber(v, &z.xl));
    if (k == "xr") return G(ReadNumber(v, &z.xr));
    if (k == "fis" || k == "fos") {  // 13-62i: a REAPER fade shape (0..6)
        int x = 0;
        if (!ReadInt(v, &x) || x < 0 || x > 6) return Got::Bad;
        (k == "fis" ? z.fade_in_shape : z.fade_out_shape) = x;
        return Got::Ok;
    }
    if (k == "fid" || k == "fod") {  // 13-75: a REAPER fade curvature (-1..1)
        double x = 0.0;
        if (!ReadNumber(v, &x) || !(x >= -1.0 && x <= 1.0)) return Got::Bad;
        (k == "fid" ? z.fade_in_dir : z.fade_out_dir) = x;
        return Got::Ok;
    }
    if (k == "as") {  // 13-62h review (R2): t, a, b, mode, threshold, offset (fix 2026-10-19: then 1 = on the samples)
        std::vector<double> n;
        if (!SplitNumbers(v, &n) || n.size() < 6 || n.size() > 7) return Got::Bad;
        if (n.size() == 7 && n[6] != 0.0 && n[6] != 1.0) return Got::Bad;
        for (double x : n)
            if (!std::isfinite(x)) return Got::Bad;
        if (n[3] != std::floor(n[3]) || n[3] < 0.0 || n[3] > 2.0) return Got::Bad;  // a SyncMode index
        z.auto_snap.set = true;
        z.auto_snap.t = n[0];
        z.auto_snap.a = n[1];
        z.auto_snap.b = n[2];
        z.auto_snap.mode = static_cast<int>(n[3]);
        z.auto_snap.thr = n[4];
        z.auto_snap.off = n[5];
        z.auto_snap.fine = n.size() == 7 && n[6] == 1.0;
        return Got::Ok;
    }
    return Got::Unknown;
}

// EditZone keeps its unknown fields as (key, value) pairs: a token without '=' (or "key=") as (token, "").
Kept ZoneKept(const EditZone& z)
{
    Kept k;
    for (const auto& kv : z.kept) k.push_back(kv.second.empty() ? kv.first : kv.first + "=" + kv.second);
    return k;
}

void ReadZoneKept(const Kept& k, EditZone& z)
{
    z.kept.clear();
    for (const std::string& tok : k) {
        const size_t eq = tok.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= tok.size()) z.kept.push_back({tok, std::string()});
        else z.kept.push_back({tok.substr(0, eq), tok.substr(eq + 1)});
    }
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

// Antho 2026-10-12 (13-8): an auto marker type's sound.
Fields AutoFields(const AutoSoundAssign& a)
{
    Fields f = {{"key", Enc(a.key)}};
    if (!a.library.empty()) f.push_back({"lib", Enc(a.library)});
    return f;
}

Got SetAuto(AutoSoundAssign& a, const std::string& k, const std::string& v)
{
    if (k == "key") return G(ReadStr(v, &a.key));
    if (k == "lib") return G(ReadStr(v, &a.library));
    return Got::Unknown;
}

// 13-15a: an assignment at Project or Character level, and a character.
Fields AssignFields(const SoundAssign& a)
{
    Fields f = {{"key", Enc(a.key)}};
    if (a.state == LibState::None) f.push_back({"lib", "none"});
    else if (a.state == LibState::Library && !a.library.empty()) f.push_back({"lib", Enc(a.library)});
    if (a.kit) f.push_back({"kit", "1"});
    return f;
}

Got SetAssign(SoundAssign& a, const std::string& k, const std::string& v)
{
    if (k == "key") return G(ReadStr(v, &a.key));
    if (k == "lib") {
        std::string lib;
        if (!ReadStr(v, &lib)) return Got::Bad;
        if (lib == "none") {
            a.state = LibState::None;
            a.library.clear();
        } else if (!lib.empty()) {
            a.state = LibState::Library;
            a.library = lib;
        }
        return Got::Ok;
    }
    if (k == "kit") return G(ReadFlag(v, &a.kit));
    return Got::Unknown;
}

Fields CharacterFields(const SoundCharacter& c)
{
    Fields f = {{"uid", Enc(c.uid)}, {"name", Enc(c.name)}, {"skel", Enc(c.skeleton)}};
    if (!c.model.empty()) f.push_back({"model", Enc(c.model)});
    if (!c.files.empty()) f.push_back({"files", JoinEncoded(c.files)});
    return f;
}

Got SetCharacter(SoundCharacter& c, const std::string& k, const std::string& v)
{
    if (k == "uid") return G(ReadStr(v, &c.uid));
    if (k == "name") return G(ReadStr(v, &c.name));
    if (k == "skel") return G(ReadStr(v, &c.skeleton));
    if (k == "model") return G(ReadStr(v, &c.model));
    if (k == "files") return G(SplitDecoded(v, &c.files));
    return Got::Unknown;
}

// A library's `kit` line.
Fields KitFields(const SoundLibrary& l)
{
    Fields f = {{"parts", JoinEncoded(l.kit)}};
    if (l.kit_offered) f.push_back({"offered", "1"});
    return f;
}

Got SetKit(SoundLibrary& l, const std::string& k, const std::string& v)
{
    if (k == "parts") return G(SplitDecoded(v, &l.kit));
    if (k == "offered") return G(ReadFlag(v, &l.kit_offered));
    return Got::Unknown;
}

bool HasKitLine(const SoundLibrary& l)
{
    return !l.kit.empty() || l.kit_offered || !l.kit_kept.empty();
}

// 13-15a: the old per-marker-type sounds become Project assignments (feet.step...). kMap is frozen
// legacy, in the old part words (feet / hands): RenameOldParts converts its output to legs / arms.
// Never edit it.
const char* MigratedTarget(const std::string& word)
{
    static const char* const kMap[][2] = {
        {"step", "feet.step"},       {"heel", "feet.step"},       {"toe", "feet.step"},
        {"lift", "feet.lift"},       {"slide", "feet.slide"},     {"slide_heel", "feet.slide"},
        {"slide_toe", "feet.slide"}, {"pivot", "feet.pivot"},     {"pivot_heel", "feet.pivot"},
        {"pivot_toe", "feet.pivot"}, {"grab", "hands.grab"},      {"release", "hands.release"},
        {"hand_pivot", "hands.hand_pivot"},
    };
    for (const auto& m : kMap)
        if (word == m[0]) return m[1];
    return nullptr;
}

std::vector<std::pair<std::string, std::string>> SortedValues(const std::vector<ValueSet>& v)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const ValueSet& x : v) out.push_back({x.group_uid, x.value_uid});
    std::sort(out.begin(), out.end());
    return out;
}

// Review 13-15a: a target is migrated only when the old entries that fold into it agree. Every
// old key of that target that is set (heel / toe / step, both sides...) must have both its sides
// set, with the same library and values; otherwise a marker that played nothing (an unset side or
// kind, whose rule has no library) would start playing the other's sound. The rules the old
// Detects filled keep their own copy, so nothing they play is lost either way.
void MigrateAutoSounds(SoundStore& s)
{
    std::vector<std::string> targets;
    for (const AutoSoundAssign& a : s.auto_sounds) {
        const size_t colon = a.key.find(':');
        if (colon == std::string::npos) continue;
        const char* t = MigratedTarget(a.key.substr(0, colon));
        if (t && std::find(targets.begin(), targets.end(), t) == targets.end()) targets.push_back(t);
    }
    for (const std::string& target : targets) {
        const AutoSoundAssign* first = nullptr;
        bool                   agree = true;
        std::vector<std::string> words;  // each word of the target that has an entry
        for (const AutoSoundAssign& a : s.auto_sounds) {
            const size_t colon = a.key.find(':');
            if (colon == std::string::npos) continue;
            const std::string word = a.key.substr(0, colon);
            const char*       t = MigratedTarget(word);
            if (!t || target != t) continue;
            if (std::find(words.begin(), words.end(), word) == words.end()) words.push_back(word);
            if (!first) first = &a;
            else if (a.library != first->library || SortedValues(a.values) != SortedValues(first->values)) agree = false;
        }
        if (!first || !agree || (first->library.empty() && first->values.empty())) continue;
        for (const std::string& w : words) {
            bool l = false, r = false;
            for (const AutoSoundAssign& a : s.auto_sounds) {
                if (a.key == w + ":L") l = true;
                if (a.key == w + ":R") r = true;
            }
            if (!l || !r) agree = false;
        }
        if (!agree) continue;
        bool have = false;
        for (const SoundAssign& x : s.project)
            if (x.key == target) have = true;
        if (have) continue;
        SoundAssign n;
        n.key = target;
        if (!first->library.empty()) {
            n.state = LibState::Library;
            n.library = first->library;
        }
        n.values = first->values;
        s.project.push_back(n);
    }
    // Unknown text read with the old lines is kept on the header (written back after it).
    for (const AutoSoundAssign& a : s.auto_sounds) {
        s.kept_lines.insert(s.kept_lines.end(), a.kept_lines.begin(), a.kept_lines.end());
    }
    s.auto_sounds.clear();
}

// Story 10-9-1: the body part words feet / hands became legs / arms. The old part word of `key`
// (alone or before a '.') to the new one; false when `key` has no old part word. The only place
// that knows the old words (with MigratedTarget's frozen map, whose output this converts).
bool RenamedPart(const std::string& key, std::string* out)
{
    static const char* const kRenamed[][2] = {{"feet", "legs"}, {"hands", "arms"}};
    for (const auto& r : kRenamed) {
        const size_t n = std::strlen(r[0]);
        if (key.compare(0, n, r[0]) == 0 && (key.size() == n || key[n] == '.')) {
            *out = r[1] + key.substr(n);
            return true;
        }
    }
    return false;
}

// One list of assignments: an old key becomes the new one, unless the list already holds the new
// one (read as such, or converted earlier in the list): then the old entry is dropped.
void RenameAssignParts(std::vector<SoundAssign>& v)
{
    std::vector<std::string> kept_keys;
    for (const SoundAssign& a : v) {
        std::string unused;
        if (!RenamedPart(a.key, &unused)) kept_keys.push_back(a.key);
    }
    std::vector<SoundAssign> out;
    for (SoundAssign& a : v) {
        std::string nk;
        if (RenamedPart(a.key, &nk)) {
            if (std::find(kept_keys.begin(), kept_keys.end(), nk) != kept_keys.end()) continue;
            kept_keys.push_back(nk);
            a.key = nk;
        }
        out.push_back(std::move(a));
    }
    v = std::move(out);
}

// Story 10-9-1: every part word of a store read (project chunk or .ravlib) in the new words:
// assign / cassign keys and kit parts. Idempotent; kits keep each part once, in order.
void RenameOldParts(SoundStore& s)
{
    RenameAssignParts(s.project);
    for (SoundCharacter& c : s.characters) RenameAssignParts(c.assigns);
    for (SoundLibrary& l : s.libraries) {
        std::vector<std::string> kit;
        for (const std::string& p : l.kit) {
            std::string np = p;
            RenamedPart(p, &np);
            if (std::find(kit.begin(), kit.end(), np) == kit.end()) kit.push_back(np);
        }
        l.kit = std::move(kit);
    }
}

Fields RuleFields(const RuleSounds& r)
{
    Fields f = {{"rid", Enc(r.rid)}};
    if (!r.library.empty()) f.push_back({"lib", Enc(r.library)});
    if (r.none) f.push_back({"none", "1"});
    return f;
}

Got SetRule(RuleSounds& r, const std::string& k, const std::string& v)
{
    if (k == "rid") return G(ReadStr(v, &r.rid));
    if (k == "lib") return G(ReadStr(v, &r.library));
    if (k == "none") return G(ReadFlag(v, &r.none));
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
    if (!d.part.empty()) f.push_back({"part", Enc(d.part)});
    return f;
}

Got SetDraw(SoundDraw& d, const std::string& k, const std::string& v)
{
    if (k == "node") return G(ReadStr(v, &d.node));
    if (k == "pick") return G(ReadInt(v, &d.pick));
    if (k == "hand") return G(ReadFlag(v, &d.hand));
    if (k == "part") return G(ReadStr(v, &d.part));
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
    AutoSoundAssign* au = nullptr;
    SoundAssign*     asg = nullptr;  // 13-15a: the project / character assign read last
    SoundCharacter*  chr = nullptr;
    for (; i < lines.size(); ++i) {
        std::vector<std::string> t;
        const std::string        kind = KindOf(lines[i], &t);
        if (kind == "auto") {
            s.auto_sounds.emplace_back();
            au = &s.auto_sounds.back();
            asg = nullptr;
            chr = nullptr;
            group = nullptr;
            lib = nullptr;
            node = nullptr;
            ReadFields(t, 1, au->kept, [&](const std::string& k, const std::string& v) { return SetAuto(*au, k, v); });
            unknown_to = &au->kept_lines;
        } else if (kind == "avalue" && au) {
            au->values.emplace_back();
            ValueSet& x = au->values.back();
            ReadFields(t, 1, x.kept, [&](const std::string& k, const std::string& v) { return SetValueSet(x, k, v); });
        } else if (kind == "assign") {
            s.project.emplace_back();
            asg = &s.project.back();
            au = nullptr;
            chr = nullptr;
            group = nullptr;
            lib = nullptr;
            node = nullptr;
            ReadFields(t, 1, asg->kept, [&](const std::string& k, const std::string& v) { return SetAssign(*asg, k, v); });
            unknown_to = &asg->kept_lines;
        } else if (kind == "asvalue" && asg && !chr) {
            asg->values.emplace_back();
            ValueSet& x = asg->values.back();
            ReadFields(t, 1, x.kept, [&](const std::string& k, const std::string& v) { return SetValueSet(x, k, v); });
        } else if (kind == "character") {
            s.characters.emplace_back();
            chr = &s.characters.back();
            asg = nullptr;
            au = nullptr;
            group = nullptr;
            lib = nullptr;
            node = nullptr;
            ReadFields(t, 1, chr->kept, [&](const std::string& k, const std::string& v) { return SetCharacter(*chr, k, v); });
            unknown_to = &chr->kept_lines;
        } else if (kind == "cassign" && chr) {
            chr->assigns.emplace_back();
            asg = &chr->assigns.back();
            ReadFields(t, 1, asg->kept, [&](const std::string& k, const std::string& v) { return SetAssign(*asg, k, v); });
            unknown_to = &asg->kept_lines;
        } else if (kind == "casvalue" && asg && chr) {
            asg->values.emplace_back();
            ValueSet& x = asg->values.back();
            ReadFields(t, 1, x.kept, [&](const std::string& k, const std::string& v) { return SetValueSet(x, k, v); });
        } else if (kind == "kit" && lib && !node) {
            ReadFields(t, 1, lib->kit_kept, [&](const std::string& k, const std::string& v) { return SetKit(*lib, k, v); });
        } else if (kind == "group") {
            s.groups.emplace_back();
            group = &s.groups.back();
            au = nullptr;
            asg = nullptr;
            chr = nullptr;
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
            au = nullptr;
            asg = nullptr;
            chr = nullptr;
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
            SetRecordBaseline(node->sound);  // 13-67b
            ReadFields(t, 1, node->sound.kept,
                       [&](const std::string& k, const std::string& v) { return SetSound(node->sound, k, v); });
        } else if (kind == "part" && node) {  // 13-62: a part's own settings
            node->sound.parts.emplace_back();
            PartSettings& ps = node->sound.parts.back();
            ReadFields(t, 1, ps.kept, [&](const std::string& k, const std::string& v) { return SetPart(ps, k, v); });
        } else if (kind == "frame" && node) {
            node->sound.frames.emplace_back();
            SoundFrame& fr = node->sound.frames.back();
            ReadFields(t, 1, fr.kept, [&](const std::string& k, const std::string& v) { return SetFrame(fr, k, v); });
        } else if (kind == "fzone" && node && node->sound.frames.empty()) {  // 13-62h: a zone before any frame: dropped
        } else if (kind == "fzone" && node) {  // 13-62h: a zone of the last frame
            EditZone z;
            bool     has_sa = false, has_sb = false;
            Kept     kept;
            ReadFields(t, 1, kept, [&](const std::string& k, const std::string& v) { return SetFzone(z, &has_sa, &has_sb, k, v); });
            if (!has_sa) z.slot_a = z.a;
            if (!has_sb) z.slot_b = z.b;
            ReadZoneKept(kept, z);
            node->sound.frames.back().zones.push_back(z);
        } else if (kind == "park" && node) {  // 13-67b: the variations of the mode last left
            Kept pk;  // its unknown fields: dropped
            ReadFields(t, 1, pk, [&](const std::string& k, const std::string& v) {
                if (k == "mode") return G(ModeFromWord(v, &node->sound.parked_mode));
                if (k == "edited") return G(ReadFlag(v, &node->sound.parked_edited));
                return Got::Unknown;
            });
        } else if (kind == "pframe" && node) {
            node->sound.parked.emplace_back();
            SoundFrame& fr = node->sound.parked.back();
            ReadFields(t, 1, fr.kept, [&](const std::string& k, const std::string& v) { return SetFrame(fr, k, v); });
        } else if (kind == "pzone" && node && node->sound.parked.empty()) {  // a zone before any parked frame: dropped
        } else if (kind == "pzone" && node) {
            EditZone z;
            bool     has_sa = false, has_sb = false;
            Kept     kept;
            ReadFields(t, 1, kept, [&](const std::string& k, const std::string& v) { return SetFzone(z, &has_sa, &has_sb, k, v); });
            if (!has_sa) z.slot_a = z.a;
            if (!has_sb) z.slot_b = z.b;
            ReadZoneKept(kept, z);
            node->sound.parked.back().zones.push_back(z);
        } else if (kind == "zone" && node) {
            node->sound.zones.emplace_back();
            SoundZone& z = node->sound.zones.back();
            ReadFields(t, 1, z.kept, [&](const std::string& k, const std::string& v) { return SetZone(z, k, v); });
        } else {
            unknown_to->push_back(lines[i]);
        }
    }
    // 13-62h: a divided Sound of an old project / .ravlib (cuts, owners, Sound-level zones, part gains) becomes
    // zones, once, on read. 13-62i: an undivided Sound's variations too (any mode, Takes included), and a divided
    // Sound's takes get proposed zones.
    // Zones read are made sane (b < a, overlaps, crossfades out of range, unknown parts): the invariant holds
    // after any read.
    for (SoundLibrary& l : s.libraries)
        for (SoundNode& n : l.nodes)
            if (n.type == SoundNodeType::Sound) {
                MigrateLegacyZones(n.sound);
                SanitizeZones(n.sound);
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
        MigrateAutoSounds(s);
        RenameOldParts(s);  // after the migration: its targets are in the old words
        *out = std::move(s);
        return true;
    } catch (...) {
        return false;
    }
}

bool SoundStoreEmpty(const SoundStore& s)
{
    return s.groups.empty() && s.libraries.empty() && s.kept.empty() && s.kept_lines.empty() && s.auto_sounds.empty() &&
           s.project.empty() && s.characters.empty();
}

std::string SerializeSoundStore(const SoundStore& s)
{
    if (SoundStoreEmpty(s)) return std::string();
    std::string out;
    WriteHeader(out, kSoundStoreMagic, s.version, {}, s.kept);
    WriteLines(out, s.kept_lines);
    // Right after the header: a version without them keeps them there (the header's unknown lines).
    for (const AutoSoundAssign& a : s.auto_sounds) {
        WriteLine(out, "auto", AutoFields(a), a.kept);
        for (const ValueSet& v : a.values) WriteLine(out, "avalue", ValueSetFields(v), v.kept);
        WriteLines(out, a.kept_lines);
    }
    auto write_assign = [&](const char* line, const char* vline, const SoundAssign& a) {
        WriteLine(out, line, AssignFields(a), a.kept);
        for (const ValueSet& v : a.values) WriteLine(out, vline, ValueSetFields(v), v.kept);
        WriteLines(out, a.kept_lines);
    };
    for (const SoundAssign& a : s.project) write_assign("assign", "asvalue", a);
    for (const SoundCharacter& c : s.characters) {
        WriteLine(out, "character", CharacterFields(c), c.kept);
        WriteLines(out, c.kept_lines);
        for (const SoundAssign& a : c.assigns) write_assign("cassign", "casvalue", a);
    }
    for (const SwitchGroup& g : s.groups) {
        WriteLine(out, "group", GroupFields(g), g.kept);
        for (const SwitchValue& v : g.values) WriteLine(out, "value", ValueFields(v), v.kept);
        WriteLines(out, g.kept_lines);
    }
    for (const SoundLibrary& l : s.libraries) {
        WriteLine(out, "library", LibraryFields(l), l.kept);
        if (HasKitLine(l)) WriteLine(out, "kit", KitFields(l), l.kit_kept);
        WriteLines(out, l.kept_lines);
        for (const SoundNode& n : l.nodes) {
            WriteLine(out, "node", NodeFields(n), n.kept);
            for (const SwitchCase& c : n.cases) WriteLine(out, "case", CaseFields(c), c.kept);
            for (const std::string& u : n.unassigned) WriteLine(out, "unassigned", {{"child", Enc(u)}}, {});
            if (HasSoundLine(n)) {
                WriteLine(out, "sound", SoundFields(n.sound), n.sound.kept);
                for (const PartSettings& ps : n.sound.parts) WriteLine(out, "part", PartFields(ps), ps.kept);  // 13-62
                for (const SoundFrame& fr : n.sound.frames) {
                    WriteLine(out, "frame", FrameFields(fr), fr.kept);
                    for (const EditZone& z : fr.zones) WriteLine(out, "fzone", FzoneFields(z), ZoneKept(z));  // 13-62h
                }
                for (const SoundZone& z : n.sound.zones) WriteLine(out, "zone", ZoneFields(z), z.kept);
                if (!n.sound.parked.empty()) {  // 13-67b
                    Fields pf = {{"mode", kModeWords[static_cast<int>(n.sound.parked_mode)]}};
                    if (n.sound.parked_edited) pf.push_back({"edited", "1"});
                    WriteLine(out, "park", pf, {});
                    for (const SoundFrame& fr : n.sound.parked) {
                        WriteLine(out, "pframe", FrameFields(fr), fr.kept);
                        for (const EditZone& z : fr.zones) WriteLine(out, "pzone", FzoneFields(z), ZoneKept(z));
                    }
                }
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
            if (k == "tk") return G(ReadStr(v, &t.tk));
            if (k == "pt") return G(ReadStr(v, &t.pt));
            if (k == "g") return G(t.has_g = ReadNumber(v, &t.g));  // 13-62i (D)
            if (k == "pi") return G(ReadInt(v, &t.pi));
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
    if (!t.tk.empty()) f.push_back({"tk", Enc(t.tk)});
    if (!t.pt.empty()) f.push_back({"pt", Enc(t.pt)});
    if (t.has_g) f.push_back({"g", FormatNumber(t.g)});  // 13-62i (D)
    if (t.pi >= 0) f.push_back({"pi", FmtInt(t.pi)});
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
