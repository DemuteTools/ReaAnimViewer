// SPDX-License-Identifier: MIT
//
// See rule_record.h.

#include "rule_record.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <mutex>
#include <system_error>

namespace rav {

// ---- Bone references -------------------------------------------------------------------

namespace {

std::mutex&               InternMutex()
{
    static std::mutex m;
    return m;
}
std::vector<std::string>& InternTable()
{
    static std::vector<std::string> t;
    return t;
}

const char kRolePrefix[] = "role:";
const char kBonePrefix[] = "bone:";

bool StartsWith(const std::string& s, const char* pre)
{
    const size_t n = std::strlen(pre);
    return s.size() >= n && s.compare(0, n, pre) == 0;
}

}  // namespace

int BoneRefId(const std::string& key)
{
    if (key.empty()) return -1;
    if (StartsWith(key, kRolePrefix)) {
        Role r;
        if (RoleFromKey(key.substr(sizeof(kRolePrefix) - 1), &r)) return static_cast<int>(r);
    }
    std::lock_guard<std::mutex> lock(InternMutex());
    std::vector<std::string>&   t = InternTable();
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i] == key) return kBoneRefExtra + static_cast<int>(i);
    t.push_back(key);
    return kBoneRefExtra + static_cast<int>(t.size() - 1);
}

int BoneRefForBone(const std::string& raw_name)
{
    return BoneRefId(kBonePrefix + raw_name);
}

std::string BoneRefKey(int id)
{
    if (id >= 0 && id < static_cast<int>(Role::Count)) return std::string(kRolePrefix) + RoleKey(static_cast<Role>(id));
    if (id < kBoneRefExtra) return "";
    std::lock_guard<std::mutex> lock(InternMutex());
    const std::vector<std::string>& t = InternTable();
    const size_t                    i = static_cast<size_t>(id - kBoneRefExtra);
    return i < t.size() ? t[i] : std::string();
}

std::string BoneRefName(int id)
{
    if (id >= 0 && id < static_cast<int>(Role::Count)) return RoleName(static_cast<Role>(id));
    const std::string key = BoneRefKey(id);
    if (StartsWith(key, kBonePrefix)) return key.substr(sizeof(kBonePrefix) - 1);
    if (StartsWith(key, kRolePrefix)) {
        std::string s = key.substr(sizeof(kRolePrefix) - 1);
        for (char& c : s)
            if (c == '_') c = ' ';
        return s;
    }
    return key.empty() ? std::string("?") : key;
}

std::string BoneRefLabel(int id)
{
    if (id >= 0 && id < static_cast<int>(Role::Count)) return RoleShortLabel(static_cast<Role>(id));
    return BoneRefName(id);
}

namespace {

// The bone lists a signal reads: ref_bones only with a bones reference (a floor-referenced
// signal ignores leftover ones, as RolesUsed does).
template <typename F>
void ForEachBoneList(std::vector<Block>& blocks, F f)
{
    auto signal = [&](SignalSpec& s) {
        f(s.bones);
        if (s.reference == Reference::Bones) f(s.ref_bones);
    };
    for (Block& b : blocks) {
        for (Condition& c : b.conditions) signal(c.signal);
        signal(b.strength_signal);
    }
}

}  // namespace

std::vector<int> BoneRefsUsed(const std::vector<Block>& blocks)
{
    std::vector<int>   out;
    std::vector<Block> copy = blocks;
    ForEachBoneList(copy, [&](std::vector<int>& ids) {
        for (int id : ids)
            if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    });
    return out;
}

namespace {

// The binding (BindBoneRefs); `names` gets what does not bind, each once.
bool BindCore(std::vector<Block>& blocks, const std::vector<int>& role_to_bone,
              const std::vector<std::string>& bone_names, const std::vector<int>& bone_parents,
              std::vector<std::string>& names)
{
    std::vector<std::string> norm;
    norm.reserve(bone_names.size());
    for (const std::string& n : bone_names) norm.push_back(NormalizeBoneName(n));

    auto resolve = [&](int id) -> int {
        if (id >= 0 && id < static_cast<int>(Role::Count)) {
            const int b = id < static_cast<int>(role_to_bone.size()) ? role_to_bone[id] : -1;
            return (b >= 0 && b < static_cast<int>(bone_names.size())) ? b : -1;
        }
        const std::string key = BoneRefKey(id);
        if (StartsWith(key, kBonePrefix)) {
            const std::string raw = key.substr(sizeof(kBonePrefix) - 1);
            for (size_t i = 0; i < bone_names.size(); ++i)
                if (bone_names[i] == raw) return static_cast<int>(i);
            const std::string n = NormalizeBoneName(raw);
            for (size_t i = 0; i < norm.size(); ++i)
                if (norm[i] == n) return static_cast<int>(i);
        }
        return -1;
    };
    auto add = [&](const std::string& nm) {
        if (std::find(names.begin(), names.end(), nm) == names.end()) names.push_back(nm);
    };
    auto list = [&](std::vector<int>& ids) {
        for (int& id : ids) {
            const int b = resolve(id);
            if (b < 0) add(BoneRefName(id));
            id = b;
        }
    };
    // 10-4 follow-up: an interior angle's record names the joint only; it binds to {its
    // parent, the joint, its first child in skeleton order}. A root or a leaf is no joint.
    auto joint = [&](std::vector<int>& ids) {
        const int id = ids[0];
        const int b = resolve(id);
        if (b < 0) {
            add(BoneRefName(id));
            ids = {-1};
            return;
        }
        const int parent = b < static_cast<int>(bone_parents.size()) ? bone_parents[static_cast<size_t>(b)] : -1;
        int child = -1;
        for (size_t i = 0; i < bone_parents.size() && child < 0; ++i)
            if (bone_parents[i] == b) child = static_cast<int>(i);
        if (parent < 0 || parent >= static_cast<int>(bone_names.size()) || child < 0 ||
            child >= static_cast<int>(bone_names.size())) {
            add(BoneRefName(id) + " (not a joint)");
            ids = {-1};
            return;
        }
        ids = {parent, b, child};
    };
    auto signal = [&](SignalSpec& s) {
        if (s.quantity == Quantity::InteriorAngle && s.bones.size() == 1) joint(s.bones);
        else list(s.bones);
        if (s.reference == Reference::Bones) list(s.ref_bones);
    };
    for (Block& b : blocks) {
        for (Condition& c : b.conditions) signal(c.signal);
        signal(b.strength_signal);
    }
    return names.empty();
}

}  // namespace

std::vector<std::string> MissingBoneRefs(const std::vector<Block>& blocks, const std::vector<int>& role_to_bone,
                                         const std::vector<std::string>& bone_names,
                                         const std::vector<int>& bone_parents)
{
    std::vector<Block>       copy = blocks;
    std::vector<std::string> names;
    BindCore(copy, role_to_bone, bone_names, bone_parents, names);
    return names;
}

bool BindBoneRefs(std::vector<Block>& blocks, const std::vector<int>& role_to_bone,
                  const std::vector<std::string>& bone_names, std::string* missing)
{
    return BindBoneRefs(blocks, role_to_bone, bone_names, std::vector<int>{}, missing);
}

bool BindBoneRefs(std::vector<Block>& blocks, const std::vector<int>& role_to_bone,
                  const std::vector<std::string>& bone_names, const std::vector<int>& bone_parents,
                  std::string* missing)
{
    std::vector<std::string> names;  // missing, each once
    BindCore(blocks, role_to_bone, bone_names, bone_parents, names);
    std::string miss;
    for (const std::string& nm : names) miss += (miss.empty() ? "" : ", ") + nm;
    if (missing) *missing = miss;
    return names.empty();
}

// ---- Numbers ---------------------------------------------------------------------------

std::string FormatNumber(double v)
{
    if (!std::isfinite(v)) return "0";
    if (v == 0.0) return "0";  // also -0
    char buf[64];
    const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), v);
    if (r.ec != std::errc()) return "0";
    return std::string(buf, r.ptr);
}

bool ReadNumber(const std::string& s, double* out)
{
    if (s.empty()) return false;
    double                       v = 0.0;
    const char*                  b = s.data();
    const char*                  e = b + s.size();
    const std::from_chars_result r = std::from_chars(b, e, v);
    if (r.ec != std::errc() || r.ptr != e || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

namespace {

bool ReadInt(const std::string& s, int* out)
{
    if (s.empty()) return false;
    int                          v = 0;
    const char*                  b = s.data();
    const char*                  e = b + s.size();
    const std::from_chars_result r = std::from_chars(b, e, v);
    if (r.ec != std::errc() || r.ptr != e) return false;
    *out = v;
    return true;
}

bool ReadBool(const std::string& s, bool* out)
{
    if (s == "0") *out = false;
    else if (s == "1") *out = true;
    else return false;
    return true;
}

std::string FormatInt(int v)
{
    char                         buf[16];
    const std::to_chars_result   r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

// ---- Keys and free text ----------------------------------------------------------------

bool NeedsEscape(unsigned char c)
{
    return c <= 0x20 || c == 0x7F || c == ',' || c == '=' || c == '%';
}

std::string EncodeKey(const std::string& key)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string       out;
    for (char ch : key) {
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

int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool DecodeKey(const std::string& s, std::string* out)
{
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size()) return false;
            const int h = HexDigit(s[i + 1]), l = HexDigit(s[i + 2]);
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

std::string FormatBones(const std::vector<int>& ids)
{
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        std::string key = BoneRefKey(ids[i]);
        if (key.empty()) key = "bone:?";
        if (i) out += ',';
        out += EncodeKey(key);
    }
    return out;
}

bool ReadBones(const std::string& s, std::vector<int>* out)
{
    std::vector<int> ids;
    if (!s.empty()) {
        size_t pos = 0;
        while (true) {
            const size_t comma = s.find(',', pos);
            const std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            std::string key;
            if (item.empty() || !DecodeKey(item, &key) || key.find(':') == std::string::npos) return false;
            ids.push_back(BoneRefId(key));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    *out = ids;
    return true;
}

std::string FormatDoubles(const std::vector<double>& v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ',';
        out += FormatNumber(v[i]);
    }
    return out;
}

bool ReadDoubleList(const std::string& s, std::vector<double>* out)
{
    std::vector<double> v;
    if (!s.empty()) {
        size_t pos = 0;
        while (true) {
            const size_t comma = s.find(',', pos);
            double d = 0.0;
            if (!ReadNumber(s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos), &d))
                return false;
            v.push_back(d);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    *out = v;
    return true;
}

// Free text on one line: no CR / LF, trimmed.
std::string CleanFreeText(const std::string& s)
{
    std::string t = s;
    for (char& c : t)
        if (c == '\r' || c == '\n') c = ' ';
    const size_t b = t.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    const size_t e = t.find_last_not_of(' ');
    return t.substr(b, e - b + 1);
}

std::string FormatColor(uint32_t c)
{
    if (c == 0) return "none";
    static const char kHex[] = "0123456789ABCDEF";
    std::string       s = "#";
    for (int sh = 20; sh >= 0; sh -= 4) s += kHex[(c >> sh) & 15];
    return s;
}

bool ReadColor(const std::string& s, uint32_t* out)
{
    if (s == "none") {
        *out = 0;
        return true;
    }
    if (s.size() != 7 || s[0] != '#') return false;
    uint32_t v = 0;
    for (size_t i = 1; i < 7; ++i) {
        const int d = HexDigit(s[i]);
        if (d < 0) return false;
        v = v * 16 + static_cast<uint32_t>(d);
    }
    *out = 0x1000000u | v;
    return true;
}

// ---- Enum words ------------------------------------------------------------------------

template <typename E>
struct Word {
    E           value;
    const char* word;
};

const Word<Quantity>  kQuantity[] = {{Quantity::Point, "point"},          {Quantity::JointAngle, "joint"},
                                     {Quantity::Yaw, "yaw"},              {Quantity::InteriorAngle, "angle"},
                                     {Quantity::Rotation, "rot"}};
const Word<Combine>   kCombine[] = {{Combine::Single, "single"}, {Combine::Average, "average"},
                                    {Combine::Lowest, "lowest"}, {Combine::Highest, "highest"}};
const Word<Reference> kReference[] = {{Reference::Floor, "floor"}, {Reference::Bones, "bones"},
                                     {Reference::Parent, "parent"}};
const Word<Measure>   kMeasure[] = {{Measure::Position, "position"}, {Measure::Speed, "speed"},
                                    {Measure::Acceleration, "accel"}};
const Word<Axis>      kAxis[] = {{Axis::Vertical, "vertical"}, {Axis::Horizontal, "horizontal"}, {Axis::Total, "total"},
                                 {Axis::X, "x"},               {Axis::Y, "y"},                   {Axis::Z, "z"}};
const Word<Direction> kDirection[] = {{Direction::Below, "below"}, {Direction::Above, "above"}};

template <typename E, size_t N>
const char* ToWord(const Word<E> (&table)[N], E v)
{
    for (const Word<E>& w : table)
        if (w.value == v) return w.word;
    return table[0].word;
}

template <typename E, size_t N>
bool FromWord(const Word<E> (&table)[N], const std::string& s, E* out)
{
    for (const Word<E>& w : table)
        if (s == w.word) {
            *out = w.value;
            return true;
        }
    return false;
}

// ---- Canonical fields ------------------------------------------------------------------

using Fields = std::vector<std::pair<std::string, std::string>>;

// A setter's verdict on one field: not a key of this line, a key whose value did not read,
// or read into the model.
enum class Set { Unknown, Bad, Ok };

Set R(bool ok)
{
    return ok ? Set::Ok : Set::Bad;
}

Fields OptionsFields(const DetectOptions& o)
{
    return {{"sensitivity", FormatNumber(o.sensitivity)},
            {"edge_ms", FormatNumber(o.edge_margin_ms)},
            {"smooth_ms", FormatNumber(o.smooth_ms)}};
}

Set SetOptions(DetectOptions& o, const std::string& k, const std::string& v)
{
    if (k == "sensitivity") return R(ReadNumber(v, &o.sensitivity));
    if (k == "edge_ms") return R(ReadNumber(v, &o.edge_margin_ms));
    if (k == "smooth_ms") return R(ReadNumber(v, &o.smooth_ms));
    return Set::Unknown;
}

Fields AnalyseFields(const AnalyseOptions& a)
{
    return {{"floor_pct", FormatNumber(a.floor_percentile)},   {"pos_frac", FormatNumber(a.position_fraction)},
            {"speed_pct", FormatNumber(a.speed_percentile)},   {"margin_ratio", FormatNumber(a.margin_ratio)},
            {"onset_frac", FormatNumber(a.onset_fraction)},    {"per_bone_floor", a.per_bone_floor ? "1" : "0"}};
}

Set SetAnalyse(AnalyseOptions& a, const std::string& k, const std::string& v)
{
    if (k == "floor_pct") return R(ReadNumber(v, &a.floor_percentile));
    if (k == "pos_frac") return R(ReadNumber(v, &a.position_fraction));
    if (k == "speed_pct") return R(ReadNumber(v, &a.speed_percentile));
    if (k == "margin_ratio") return R(ReadNumber(v, &a.margin_ratio));
    if (k == "onset_frac") return R(ReadNumber(v, &a.onset_fraction));
    if (k == "per_bone_floor") return R(ReadBool(v, &a.per_bone_floor));
    return Set::Unknown;
}

struct PresetHeader {
    std::string id;
    int         version = 1;
    int         kept = 0;
    std::string name;
};

Fields PresetFields(const PresetHeader& h, bool with_kept)
{
    Fields f = {{"id", EncodeKey(h.id)}, {"version", FormatInt(h.version)}};
    if (with_kept) f.push_back({"kept", FormatInt(h.kept)});
    f.push_back({"name", CleanFreeText(h.name)});
    return f;
}

Set SetPreset(PresetHeader& h, bool with_kept, const std::string& k, const std::string& v)
{
    if (k == "id") return R(DecodeKey(v, &h.id));
    if (k == "version") return R(ReadInt(v, &h.version));
    if (with_kept && k == "kept") return R(ReadInt(v, &h.kept));
    if (k == "name") {
        h.name = v;
        return Set::Ok;
    }
    return Set::Unknown;
}

std::string FormatLanding(const Block& b)
{
    if (b.landing == Landing::Crossing) return "cross";
    return "peak:" + FormatInt(b.peak_condition) + (b.peak_max ? ":max" : ":min");
}

bool ReadLanding(const std::string& s, Block& b)
{
    if (s == "cross") {
        b.landing = Landing::Crossing;
        return true;
    }
    if (s.compare(0, 5, "peak:") != 0) return false;
    const size_t c = s.find(':', 5);
    if (c == std::string::npos) return false;
    int idx = 0;
    if (!ReadInt(s.substr(5, c - 5), &idx)) return false;
    const std::string mode = s.substr(c + 1);
    if (mode != "max" && mode != "min") return false;
    b.landing = Landing::PeakOf;
    b.peak_condition = idx;
    b.peak_max = (mode == "max");
    return true;
}

Fields BlockFields(const Block& b)
{
    // `on` is written only when the rule is off (story 10-3), so a record of rules that are
    // all on writes back byte-identical.
    Fields f;
    if (!b.enabled) f.push_back({"on", "0"});
    Fields rest = {{"color", FormatColor(b.color)},
            {"hold_ms", FormatNumber(b.min_hold_ms)},
            {"cooldown_ms", FormatNumber(b.cooldown_ms)},
            {"offset_ms", FormatNumber(b.offset_ms)},
            {"land", FormatLanding(b)},
            {"marker", CleanFreeText(b.marker)}};
    f.insert(f.end(), rest.begin(), rest.end());
    return f;
}

Set SetBlock(Block& b, const std::string& k, const std::string& v)
{
    if (k == "on") return R(ReadBool(v, &b.enabled));
    if (k == "color") return R(ReadColor(v, &b.color));
    if (k == "hold_ms") return R(ReadNumber(v, &b.min_hold_ms));
    if (k == "cooldown_ms") return R(ReadNumber(v, &b.cooldown_ms));
    if (k == "offset_ms") return R(ReadNumber(v, &b.offset_ms));
    if (k == "land") return R(ReadLanding(v, b));
    if (k == "marker") {
        b.marker = v;
        return Set::Ok;
    }
    return Set::Unknown;
}

void AppendSignalFields(Fields& f, const SignalSpec& s)
{
    f.push_back({"q", ToWord(kQuantity, s.quantity)});
    f.push_back({"bones", FormatBones(s.bones)});
    f.push_back({"comb", ToWord(kCombine, s.combine)});
    f.push_back({"ref", ToWord(kReference, s.reference)});
    if (s.reference == Reference::Bones || !s.ref_bones.empty()) f.push_back({"ref_bones", FormatBones(s.ref_bones)});
    if (s.reference == Reference::Bones || s.ref_combine != Combine::Average)
        f.push_back({"ref_comb", ToWord(kCombine, s.ref_combine)});
    if (s.floor_y != 0.0) f.push_back({"floor", FormatNumber(s.floor_y)});
    if (!s.bone_floors.empty()) f.push_back({"floors", FormatDoubles(s.bone_floors)});
    f.push_back({"meas", ToWord(kMeasure, s.measure)});
    f.push_back({"axis", ToWord(kAxis, s.axis)});
    if (s.keep_sign) f.push_back({"signed", "1"});
}

Set SetSignal(SignalSpec& s, const std::string& k, const std::string& v)
{
    if (k == "q") return R(FromWord(kQuantity, v, &s.quantity));
    if (k == "bones") return R(ReadBones(v, &s.bones));
    if (k == "comb") return R(FromWord(kCombine, v, &s.combine));
    if (k == "ref") return R(FromWord(kReference, v, &s.reference));
    if (k == "ref_bones") return R(ReadBones(v, &s.ref_bones));
    if (k == "ref_comb") return R(FromWord(kCombine, v, &s.ref_combine));
    if (k == "floor") return R(ReadNumber(v, &s.floor_y));
    if (k == "floors") return R(ReadDoubleList(v, &s.bone_floors));
    if (k == "meas") return R(FromWord(kMeasure, v, &s.measure));
    if (k == "axis") return R(FromWord(kAxis, v, &s.axis));
    if (k == "signed") return R(ReadBool(v, &s.keep_sign));
    return Set::Unknown;
}

Fields CondFields(const Condition& c)
{
    Fields f;
    AppendSignalFields(f, c.signal);
    f.push_back({"dir", ToWord(kDirection, c.dir)});
    f.push_back({"thr", FormatNumber(c.threshold)});
    f.push_back({"margin", FormatNumber(c.margin)});
    f.push_back({"fixed", c.auto_threshold ? "0" : "1"});
    return f;
}

Set SetCond(Condition& c, const std::string& k, const std::string& v)
{
    if (k == "dir") return R(FromWord(kDirection, v, &c.dir));
    if (k == "thr") return R(ReadNumber(v, &c.threshold));
    if (k == "margin") return R(ReadNumber(v, &c.margin));
    if (k == "fixed") {
        bool fixed = false;
        if (!ReadBool(v, &fixed)) return Set::Bad;
        c.auto_threshold = !fixed;
        return Set::Ok;
    }
    return SetSignal(c.signal, k, v);
}

// The strength line is written when it says something: non-default values, or kept text.
bool HasStrengthLine(const Block& b)
{
    const Block def;
    return !SignalsEqual(b.strength_signal, def.strength_signal) || b.strength_sign != def.strength_sign ||
           b.strength_window_ms != def.strength_window_ms || !b.kept_strength.empty();
}

Fields StrengthFields(const Block& b)
{
    Fields f;
    AppendSignalFields(f, b.strength_signal);
    f.push_back({"sign", FormatNumber(b.strength_sign)});
    f.push_back({"window_ms", FormatNumber(b.strength_window_ms)});
    return f;
}

Set SetStrength(Block& b, const std::string& k, const std::string& v)
{
    if (k == "sign") return R(ReadNumber(v, &b.strength_sign));
    if (k == "window_ms") return R(ReadNumber(v, &b.strength_window_ms));
    return SetSignal(b.strength_signal, k, v);
}

// ---- Story 10-4 lines ------------------------------------------------------------------------

const Word<EventKind>  kEventKind[] = {{EventKind::Detected, "detected"}, {EventKind::User, "user"},
                                       {EventKind::Suppress, "suppress"}};
const Word<MarkerMode> kMarkerMode[] = {{MarkerMode::Take, "take"}, {MarkerMode::Project, "project"},
                                        {MarkerMode::Both, "both"}};

// `t`, then `kind` (absent for a kind this version does not know: its raw text is kept),
// then the optional fields the entry carries.
Fields EventFields(const EventEntry& e)
{
    Fields f = {{"t", FormatNumber(e.t)}};
    if (e.kind != EventKind::Other) f.push_back({"kind", ToWord(kEventKind, e.kind)});
    if (e.has_block) f.push_back({"block", FormatInt(e.block)});
    if (e.has_strength) f.push_back({"strength", FormatNumber(e.strength)});
    if (e.has_speed) f.push_back({"speed", FormatNumber(e.speed)});
    return f;
}

Set SetEvent(EventEntry& e, const std::string& k, const std::string& v)
{
    if (k == "t") return R(ReadNumber(v, &e.t));
    if (k == "kind") return R(FromWord(kEventKind, v, &e.kind));
    if (k == "block") return R(e.has_block = ReadInt(v, &e.block));
    if (k == "strength") return R(e.has_strength = ReadNumber(v, &e.strength));
    if (k == "speed") return R(e.has_speed = ReadNumber(v, &e.speed));
    return Set::Unknown;
}

Fields AppliedFields(const AppliedInfo& a)
{
    return {{"markers", ToWord(kMarkerMode, a.mode)}, {"sig", EncodeKey(a.sig)}};
}

Set SetApplied(AppliedInfo& a, const std::string& k, const std::string& v)
{
    if (k == "markers") return R(FromWord(kMarkerMode, v, &a.mode));
    if (k == "sig") return R(DecodeKey(v, &a.sig));
    return Set::Unknown;
}

Fields TakeMarkerFields(const TakeMarkerRef& m)
{
    return {{"t", FormatNumber(m.t)}, {"name", CleanFreeText(m.name)}};
}

Set SetTakeMarkerRef(TakeMarkerRef& m, const std::string& k, const std::string& v)
{
    if (k == "t") return R(ReadNumber(v, &m.t));
    if (k == "name") {
        m.name = v;
        return Set::Ok;
    }
    return Set::Unknown;
}

Fields ProjectMarkerFields(const ProjectMarkerRef& m)
{
    return {{"guid", EncodeKey(m.guid)}, {"t", FormatNumber(m.t)}};
}

Set SetProjectMarkerRef(ProjectMarkerRef& m, const std::string& k, const std::string& v)
{
    if (k == "guid") return R(DecodeKey(v, &m.guid));
    if (k == "t") return R(ReadNumber(v, &m.t));
    return Set::Unknown;
}

Set SetNothing(const std::string&, const std::string&)
{
    return Set::Unknown;
}

// ---- Line tokenizer --------------------------------------------------------------------

bool IsKeyChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// A token "key=..." with a non-empty [A-Za-z0-9_] key: the key length, else 0.
size_t FieldKeyLength(const std::string& tok)
{
    const size_t eq = tok.find('=');
    if (eq == std::string::npos || eq == 0) return 0;
    for (size_t i = 0; i < eq; ++i)
        if (!IsKeyChar(tok[i])) return 0;
    return eq;
}

bool IsFreeTextKey(const std::string& kind_word, const std::string& key)
{
    return (kind_word == "block" && key == "marker") || (kind_word == "preset" && key == "name") ||
           ((kind_word == "tmarker" || kind_word == "ptmarker") && key == "name");
}

// The fields after the kind word. A token that is not "key=value" joins the previous
// value (one space), so values with spaces and doubled spaces come back as written. A
// free-text key takes the rest of the line.
Fields Tokenize(const std::string& kind_word, const std::string& rest)
{
    Fields f;
    size_t pos = 0;
    while (pos <= rest.size()) {
        size_t sp = rest.find(' ', pos);
        if (sp == std::string::npos) sp = rest.size();
        const std::string tok = rest.substr(pos, sp - pos);
        const size_t      kl = FieldKeyLength(tok);
        if (kl > 0) {
            const std::string key = tok.substr(0, kl);
            if (IsFreeTextKey(kind_word, key)) {
                f.push_back({key, rest.substr(pos + kl + 1)});
                break;
            }
            f.push_back({key, tok.substr(kl + 1)});
        } else if (!f.empty()) {
            f.back().second += ' ';
            f.back().second += tok;
        } else {
            f.push_back({"", tok});
        }
        if (sp >= rest.size()) break;
        pos = sp + 1;
    }
    return f;
}

// Reads one known line's fields into the model through `set`. When one is not understood,
// `kept` gets every field in its order; an unreadable known field also gets the model's
// value for it right after the read (`canon` is called once the line is read).
template <typename SetF, typename CanonF>
void ReadFields(const std::string& word, const std::string& rest, bool has_rest, KeptText& kept, SetF set,
                CanonF canon)
{
    kept.fields.clear();
    if (!has_rest) return;
    const Fields           f = Tokenize(word, rest);
    std::vector<KeptField> kf(f.size());
    bool                   all = true;
    for (size_t i = 0; i < f.size(); ++i) {
        kf[i].key = f[i].first;
        kf[i].raw = f[i].second;
        const Set r = f[i].first.empty() ? Set::Unknown : set(f[i].first, f[i].second);
        kf[i].known = r != Set::Unknown;
        kf[i].read = r == Set::Ok;
        if (r != Set::Ok) all = false;
    }
    if (all) return;
    // A key written twice, once unreadable and once read: the model holds the read value.
    for (KeptField& k : kf)
        if (k.known && !k.read)
            for (const KeptField& o : kf)
                if (o.read && o.key == k.key) k.read = true;
    const Fields now = canon();
    for (KeptField& k : kf) {
        if (!k.known || k.read) continue;
        for (const auto& c : now)
            if (c.first == k.key) {
                k.parsed_present = true;
                k.parsed = c.second;
                break;
            }
    }
    kept.fields = std::move(kf);
}

// ---- Writer ----------------------------------------------------------------------------

// A known line: the fields it was read with, in their order, when it kept some (a known
// one from the model; an unreadable one raw while the model still holds its read value),
// then the model's other fields, the free-text field last. Then its kept lines.
void WriteLine(std::string& out, const char* word, const Fields& canon, const KeptText* k)
{
    out += word;
    std::vector<char> used(canon.size(), 0);
    const std::string free_key = (!canon.empty() && IsFreeTextKey(word, canon.back().first)) ? canon.back().first : "";
    if (k) {
        std::vector<std::string> done;  // known keys written (a duplicate is written once)
        for (const KeptField& f : k->fields) {
            if (f.key.empty()) {
                out += ' ' + f.raw;
                continue;
            }
            if (!f.known) {
                out += ' ' + f.key + '=' + f.raw;
                continue;
            }
            if (f.key == free_key) continue;  // written last
            if (std::find(done.begin(), done.end(), f.key) != done.end()) continue;
            done.push_back(f.key);
            size_t j = 0;
            while (j < canon.size() && (used[j] || canon[j].first != f.key)) ++j;
            const bool now = j < canon.size();
            if (now) used[j] = 1;
            if (!f.read && now == f.parsed_present && (!now || canon[j].second == f.parsed))
                out += ' ' + f.key + '=' + f.raw;  // unchanged since the read: as written
            else if (now)
                out += ' ' + f.key + '=' + canon[j].second;
            // else: the model no longer writes this field
        }
    }
    for (size_t j = 0; j < canon.size(); ++j) {
        if (used[j] || canon[j].first == free_key) continue;
        out += ' ' + canon[j].first + '=' + canon[j].second;
    }
    if (!free_key.empty()) out += ' ' + free_key + '=' + canon.back().second;
    out += '\n';
    if (k)
        for (const std::string& l : k->lines) out += l + '\n';
}

void WriteBlocks(std::string& out, const std::vector<Block>& blocks)
{
    for (const Block& b : blocks) {
        WriteLine(out, "block", BlockFields(b), &b.kept);
        for (const Condition& c : b.conditions) WriteLine(out, "cond", CondFields(c), &c.kept);
        if (HasStrengthLine(b)) WriteLine(out, "strength", StrengthFields(b), &b.kept_strength);
    }
}

void WriteHeader(std::string& out, const char* magic, int version, const std::string& rest, const KeptText& head)
{
    out += magic;
    out += ' ';
    out += FormatInt(version < 1 ? 1 : version);
    out += rest;
    out += '\n';
    for (const std::string& l : head.lines) out += l + '\n';
}

// ---- Parser ----------------------------------------------------------------------------

std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    size_t                   pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        pos = nl + 1;
    }
    return lines;
}

// The parse target: an item record (`item`) or a preset file (`preset`).
struct Target {
    ItemRules*  item = nullptr;
    PresetData* preset = nullptr;
};

bool ParseCore(const std::string& text_in, const char* magic, Target t)
{
    // NUL ends the record (a P_EXT buffer may carry one).
    std::string text = text_in.substr(0, text_in.find('\0'));
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    const std::vector<std::string> lines = SplitLines(text);

    size_t first = 0;
    while (first < lines.size() && lines[first].find_first_not_of(" \t") == std::string::npos) ++first;
    if (first >= lines.size()) return false;
    int         version = 1;
    std::string header_rest;
    {
        const std::string& h = lines[first];
        const size_t       ml = std::strlen(magic);
        if (h.compare(0, ml, magic) != 0 || (h.size() > ml && h[ml] != ' ')) return false;
        size_t b = ml;
        while (b < h.size() && h[b] == ' ') ++b;
        size_t e = b;
        while (e < h.size() && h[e] >= '0' && h[e] <= '9') ++e;
        if (e > b && e - b < 9) version = std::atoi(h.substr(b, e - b).c_str());
        else e = b;  // no version read: what follows is kept whole
        if (version < 1) version = 1;
        header_rest = h.substr(e);
        if (e == b && !header_rest.empty()) header_rest = ' ' + header_rest;
    }

    const bool          item = t.item != nullptr;
    DetectOptions&      options = item ? t.item->options : t.preset->options;
    AnalyseOptions&     analyse = item ? t.item->analyse : t.preset->analyse;
    std::vector<Block>& own_blocks = item ? t.item->blocks : t.preset->blocks;
    KeptText&           head = item ? t.item->head : t.preset->head;
    PresetCopy*         copy = item ? &t.item->preset_copy : nullptr;
    if (item) {
        t.item->format_version = version;
        t.item->header_rest = header_rest;
    } else {
        t.preset->format_version = version;
        t.preset->header_rest = header_rest;
    }

    PresetHeader ph;
    bool         has_preset = false;
    bool seen_options = false, seen_analyse = false, seen_copy_options = false, seen_copy_analyse = false;
    bool seen_copy = false, in_copy = false;

    // Where an unknown line goes: the object whose line it followed. Resolved when used
    // (the vectors grow).
    enum class At { Head, Options, Analyse, Preset, Copy, CopyOptions, CopyAnalyse, End, Block, Cond, Strength, Tail,
                    Applied, TMarker, PMarker, Previewed, PTMarker, PPMarker };
    At   at = At::Head;
    bool cur_in_copy = false;  // the current block's list
    int  cur_block = -1;
    int  cur_cond = -1;
    bool cur_strength = false;
    bool seen_applied = false;
    bool seen_previewed = false;

    auto blocks_of = [&](bool in_c) -> std::vector<Block>& { return in_c ? copy->blocks : own_blocks; };
    auto anchor = [&]() -> KeptText* {
        switch (at) {
        case At::Head: return &head;
        case At::Options: return &options.kept;
        case At::Analyse: return &analyse.kept;
        case At::Preset: return item ? &copy->kept : &t.preset->kept;
        case At::Copy: return &copy->copy_kept;
        case At::CopyOptions: return &copy->options.kept;
        case At::CopyAnalyse: return &copy->analyse.kept;
        case At::End: return &copy->end_kept;
        case At::Block: return &blocks_of(cur_in_copy)[cur_block].kept;
        case At::Cond: return &blocks_of(cur_in_copy)[cur_block].conditions[cur_cond].kept;
        case At::Strength: return &blocks_of(cur_in_copy)[cur_block].kept_strength;
        case At::Tail: return nullptr;
        case At::Applied: return &t.item->applied.kept;
        case At::TMarker: return &t.item->tmarkers.back().kept;
        case At::PMarker: return &t.item->pmarkers.back().kept;
        case At::Previewed: return &t.item->previewed.kept;
        case At::PTMarker: return &t.item->ptmarkers.back().kept;
        case At::PPMarker: return &t.item->ppmarkers.back().kept;
        }
        return &head;
    };
    auto leave_block = [&]() { cur_block = -1; };

    for (size_t li = first + 1; li < lines.size(); ++li) {
        const std::string& line = lines[li];
        const size_t       sp = line.find(' ');
        const std::string  word = line.substr(0, sp);
        const bool         has_rest = sp != std::string::npos;
        const std::string  rest = has_rest ? line.substr(sp + 1) : std::string();

        if (word == "options" && !(in_copy ? seen_copy_options : seen_options)) {
            (in_copy ? seen_copy_options : seen_options) = true;
            DetectOptions& o = in_copy ? copy->options : options;
            leave_block();
            at = in_copy ? At::CopyOptions : At::Options;
            ReadFields(word, rest, has_rest, o.kept, [&](const std::string& k, const std::string& v) { return SetOptions(o, k, v); },
                       [&] { return OptionsFields(o); });
        } else if (word == "analyse" && !(in_copy ? seen_copy_analyse : seen_analyse)) {
            (in_copy ? seen_copy_analyse : seen_analyse) = true;
            AnalyseOptions& a = in_copy ? copy->analyse : analyse;
            leave_block();
            at = in_copy ? At::CopyAnalyse : At::Analyse;
            ReadFields(word, rest, has_rest, a.kept, [&](const std::string& k, const std::string& v) { return SetAnalyse(a, k, v); },
                       [&] { return AnalyseFields(a); });
        } else if (word == "preset" && !has_preset && !in_copy) {
            has_preset = true;
            leave_block();
            at = At::Preset;
            KeptText& k = item ? copy->kept : t.preset->kept;
            ReadFields(word, rest, has_rest, k,
                       [&](const std::string& key, const std::string& v) { return SetPreset(ph, item, key, v); },
                       [&] { return PresetFields(ph, item); });
        } else if (item && word == "copy" && has_preset && !seen_copy && !in_copy) {
            seen_copy = in_copy = true;
            leave_block();
            at = At::Copy;
            ReadFields(word, rest, has_rest, copy->copy_kept, SetNothing, [] { return Fields(); });
        } else if (item && word == "end" && in_copy) {
            in_copy = false;
            leave_block();
            at = At::End;
            ReadFields(word, rest, has_rest, copy->end_kept, SetNothing, [] { return Fields(); });
        } else if (word == "block") {
            std::vector<Block>& list = blocks_of(in_copy);
            list.emplace_back();
            cur_in_copy = in_copy;
            cur_block = static_cast<int>(list.size()) - 1;
            cur_cond = -1;
            cur_strength = false;
            at = At::Block;
            Block& b = list.back();
            ReadFields(word, rest, has_rest, b.kept, [&](const std::string& k, const std::string& v) { return SetBlock(b, k, v); },
                       [&] { return BlockFields(b); });
        } else if (word == "cond" && cur_block >= 0) {
            Block& b = blocks_of(cur_in_copy)[cur_block];
            b.conditions.emplace_back();
            cur_cond = static_cast<int>(b.conditions.size()) - 1;
            at = At::Cond;
            Condition& c = b.conditions.back();
            ReadFields(word, rest, has_rest, c.kept, [&](const std::string& k, const std::string& v) { return SetCond(c, k, v); },
                       [&] { return CondFields(c); });
        } else if (word == "strength" && cur_block >= 0 && !cur_strength) {
            cur_strength = true;
            at = At::Strength;
            Block& b = blocks_of(cur_in_copy)[cur_block];
            ReadFields(word, rest, has_rest, b.kept_strength,
                       [&](const std::string& k, const std::string& v) { return SetStrength(b, k, v); },
                       [&] { return StrengthFields(b); });
        } else if (item && word == "event" && !in_copy) {
            leave_block();
            at = At::Tail;
            EventEntry e;
            e.kind = EventKind::Other;  // until its `kind` reads
            e.has_block = e.has_strength = e.has_speed = false;
            t.item->events.push_back(e);
            EventEntry& ev = t.item->events.back();
            ReadFields(word, rest, has_rest, ev.kept,
                       [&](const std::string& k, const std::string& v) { return SetEvent(ev, k, v); },
                       [&] { return EventFields(ev); });
        } else if (item && word == "applied" && !in_copy && !seen_applied) {
            seen_applied = true;
            leave_block();
            at = At::Applied;
            t.item->has_applied = true;
            AppliedInfo& a = t.item->applied;
            ReadFields(word, rest, has_rest, a.kept,
                       [&](const std::string& k, const std::string& v) { return SetApplied(a, k, v); },
                       [&] { return AppliedFields(a); });
        } else if (item && word == "tmarker" && !in_copy) {
            leave_block();
            at = At::TMarker;
            t.item->tmarkers.emplace_back();
            TakeMarkerRef& m = t.item->tmarkers.back();
            ReadFields(word, rest, has_rest, m.kept,
                       [&](const std::string& k, const std::string& v) { return SetTakeMarkerRef(m, k, v); },
                       [&] { return TakeMarkerFields(m); });
        } else if (item && word == "pmarker" && !in_copy) {
            leave_block();
            at = At::PMarker;
            t.item->pmarkers.emplace_back();
            ProjectMarkerRef& m = t.item->pmarkers.back();
            ReadFields(word, rest, has_rest, m.kept,
                       [&](const std::string& k, const std::string& v) { return SetProjectMarkerRef(m, k, v); },
                       [&] { return ProjectMarkerFields(m); });
        } else if (item && word == "preview" && !in_copy && !seen_previewed) {
            seen_previewed = true;
            leave_block();
            at = At::Previewed;
            t.item->has_previewed = true;
            AppliedInfo& a = t.item->previewed;
            ReadFields(word, rest, has_rest, a.kept,
                       [&](const std::string& k, const std::string& v) { return SetApplied(a, k, v); },
                       [&] { return AppliedFields(a); });
        } else if (item && word == "ptmarker" && !in_copy) {
            leave_block();
            at = At::PTMarker;
            t.item->ptmarkers.emplace_back();
            TakeMarkerRef& m = t.item->ptmarkers.back();
            ReadFields(word, rest, has_rest, m.kept,
                       [&](const std::string& k, const std::string& v) { return SetTakeMarkerRef(m, k, v); },
                       [&] { return TakeMarkerFields(m); });
        } else if (item && word == "ppmarker" && !in_copy) {
            leave_block();
            at = At::PPMarker;
            t.item->ppmarkers.emplace_back();
            ProjectMarkerRef& m = t.item->ppmarkers.back();
            ReadFields(word, rest, has_rest, m.kept,
                       [&](const std::string& k, const std::string& v) { return SetProjectMarkerRef(m, k, v); },
                       [&] { return ProjectMarkerFields(m); });
        } else if (KeptText* k = anchor()) {
            k->lines.push_back(line);
        } else {
            t.item->tail.push_back({t.item->events.size(), line});
        }
    }

    analyse.smooth_ms = options.smooth_ms;
    if (item) {
        t.item->has_preset = has_preset;
        copy->id = ph.id;
        copy->version = ph.version;
        copy->kept_version = ph.kept;
        copy->name = ph.name;
        copy->analyse.smooth_ms = copy->options.smooth_ms;
    } else {
        t.preset->id = ph.id;
        t.preset->version = ph.version;
        t.preset->name = ph.name;
    }
    return true;
}

bool BlocksKeep(const std::vector<Block>& blocks)
{
    for (const Block& b : blocks) {
        if (!b.kept.empty() || !b.kept_strength.empty()) return true;
        for (const Condition& c : b.conditions)
            if (!c.kept.empty()) return true;
    }
    return false;
}

}  // namespace

// ---- Public parse / serialize ----------------------------------------------------------

bool ParseItemRules(const std::string& text, ItemRules* out)
{
    if (!out) return false;
    try {
        ItemRules r;
        Target    t;
        t.item = &r;
        if (!ParseCore(text, kRulesMagic, t)) return false;
        *out = std::move(r);
        return true;
    } catch (...) {
        return false;
    }
}

std::string SerializeItemRules(const ItemRules& r)
{
    std::string out;
    WriteHeader(out, kRulesMagic, r.format_version, r.header_rest, r.head);
    WriteLine(out, "options", OptionsFields(r.options), &r.options.kept);
    WriteLine(out, "analyse", AnalyseFields(r.analyse), &r.analyse.kept);
    if (r.has_preset) {
        const PresetCopy& c = r.preset_copy;
        PresetHeader      h;
        h.id = c.id;
        h.version = c.version;
        h.kept = c.kept_version;
        h.name = c.name;
        WriteLine(out, "preset", PresetFields(h, true), &c.kept);
        WriteLine(out, "copy", {}, &c.copy_kept);
        WriteLine(out, "options", OptionsFields(c.options), &c.options.kept);
        WriteLine(out, "analyse", AnalyseFields(c.analyse), &c.analyse.kept);
        WriteBlocks(out, c.blocks);
        WriteLine(out, "end", {}, &c.end_kept);
    }
    WriteBlocks(out, r.blocks);
    const size_t n = r.events.size();
    for (size_t i = 0; i < n; ++i) {
        WriteLine(out, "event", EventFields(r.events[i]), &r.events[i].kept);
        for (const RecordTailLine& l : r.tail)
            if (l.after_events == i + 1) out += l.text + '\n';
    }
    for (const RecordTailLine& l : r.tail)
        if (l.after_events > n || l.after_events == 0) out += l.text + '\n';
    if (r.has_applied) WriteLine(out, "applied", AppliedFields(r.applied), &r.applied.kept);
    for (const TakeMarkerRef& m : r.tmarkers) WriteLine(out, "tmarker", TakeMarkerFields(m), &m.kept);
    for (const ProjectMarkerRef& m : r.pmarkers) WriteLine(out, "pmarker", ProjectMarkerFields(m), &m.kept);
    if (r.has_previewed) WriteLine(out, "preview", AppliedFields(r.previewed), &r.previewed.kept);
    for (const TakeMarkerRef& m : r.ptmarkers) WriteLine(out, "ptmarker", TakeMarkerFields(m), &m.kept);
    for (const ProjectMarkerRef& m : r.ppmarkers) WriteLine(out, "ppmarker", ProjectMarkerFields(m), &m.kept);
    return out;
}

bool ParsePreset(const std::string& text, PresetData* out)
{
    if (!out) return false;
    try {
        PresetData d;
        Target     t;
        t.preset = &d;
        if (!ParseCore(text, kPresetMagic, t)) return false;
        *out = std::move(d);
        return true;
    } catch (...) {
        return false;
    }
}

std::string SerializePreset(const PresetData& d)
{
    std::string out;
    WriteHeader(out, kPresetMagic, d.format_version, d.header_rest, d.head);
    PresetHeader h;
    h.id = d.id;
    h.version = d.version;
    h.name = d.name;
    WriteLine(out, "preset", PresetFields(h, false), &d.kept);
    WriteLine(out, "options", OptionsFields(d.options), &d.options.kept);
    WriteLine(out, "analyse", AnalyseFields(d.analyse), &d.analyse.kept);
    WriteBlocks(out, d.blocks);
    return out;
}

bool HasKeptText(const ItemRules& r)
{
    const PresetCopy& c = r.preset_copy;
    bool story4 = !r.applied.kept.empty();
    for (const EventEntry& e : r.events)
        if (!e.kept.empty()) story4 = true;
    for (const TakeMarkerRef& m : r.tmarkers)
        if (!m.kept.empty()) story4 = true;
    for (const ProjectMarkerRef& m : r.pmarkers)
        if (!m.kept.empty()) story4 = true;
    if (!r.previewed.kept.empty()) story4 = true;
    for (const TakeMarkerRef& m : r.ptmarkers)
        if (!m.kept.empty()) story4 = true;
    for (const ProjectMarkerRef& m : r.ppmarkers)
        if (!m.kept.empty()) story4 = true;
    return story4 || !r.header_rest.empty() || !r.head.empty() || !r.tail.empty() || !r.options.kept.empty() || !r.analyse.kept.empty() ||
           BlocksKeep(r.blocks) ||
           (r.has_preset && (!c.kept.empty() || !c.copy_kept.empty() || !c.end_kept.empty() ||
                             !c.options.kept.empty() || !c.analyse.kept.empty() || BlocksKeep(c.blocks)));
}

bool HasKeptText(const PresetData& d)
{
    return !d.header_rest.empty() || !d.head.empty() || !d.kept.empty() || !d.options.kept.empty() || !d.analyse.kept.empty() ||
           BlocksKeep(d.blocks);
}

const char* MarkerModeWord(MarkerMode m)
{
    return ToWord(kMarkerMode, m);
}

bool MarkerModeFromWord(const std::string& s, MarkerMode* out)
{
    MarkerMode m;
    if (!FromWord(kMarkerMode, s, &m)) return false;
    if (out) *out = m;
    return true;
}

// ---- Equality --------------------------------------------------------------------------

bool OptionsEqual(const DetectOptions& a, const DetectOptions& b)
{
    return a.sensitivity == b.sensitivity && a.edge_margin_ms == b.edge_margin_ms && a.smooth_ms == b.smooth_ms;
}

bool AnalyseEqual(const AnalyseOptions& a, const AnalyseOptions& b)
{
    return a.floor_percentile == b.floor_percentile && a.position_fraction == b.position_fraction &&
           a.speed_percentile == b.speed_percentile && a.margin_ratio == b.margin_ratio &&
           a.onset_fraction == b.onset_fraction && a.per_bone_floor == b.per_bone_floor;
}

bool SignalsEqual(const SignalSpec& a, const SignalSpec& b)
{
    return a.quantity == b.quantity && a.bones == b.bones && a.combine == b.combine && a.reference == b.reference &&
           a.ref_bones == b.ref_bones && a.ref_combine == b.ref_combine && a.floor_y == b.floor_y &&
           a.bone_floors == b.bone_floors && a.measure == b.measure && a.axis == b.axis && a.keep_sign == b.keep_sign;
}

namespace {

bool ConditionsEqual(const Condition& a, const Condition& b)
{
    return SignalsEqual(a.signal, b.signal) && a.dir == b.dir && a.threshold == b.threshold && a.margin == b.margin &&
           a.auto_threshold == b.auto_threshold;
}

bool BlockEqual(const Block& a, const Block& b)
{
    if (a.enabled != b.enabled || a.marker != b.marker || a.color != b.color || a.min_hold_ms != b.min_hold_ms || a.cooldown_ms != b.cooldown_ms ||
        a.offset_ms != b.offset_ms || !SignalsEqual(a.strength_signal, b.strength_signal) ||
        a.strength_sign != b.strength_sign || a.strength_window_ms != b.strength_window_ms || a.landing != b.landing ||
        a.conditions.size() != b.conditions.size())
        return false;
    if (a.landing == Landing::PeakOf && (a.peak_condition != b.peak_condition || a.peak_max != b.peak_max)) return false;
    for (size_t i = 0; i < a.conditions.size(); ++i)
        if (!ConditionsEqual(a.conditions[i], b.conditions[i])) return false;
    return true;
}

}  // namespace

bool BlocksEqual(const std::vector<Block>& a, const std::vector<Block>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!BlockEqual(a[i], b[i])) return false;
    return true;
}

// ---- Names -----------------------------------------------------------------------------

std::string SignalBoneLabel(const SignalSpec& spec)
{
    const std::vector<int>& b = spec.bones;
    if (b.empty()) return "";
    auto side_of = [](int id) -> char {
        return (id >= 0 && id < static_cast<int>(Role::Count)) ? RoleSide(static_cast<Role>(id)) : 0;
    };
    auto is_role = [](int id, Role a, Role c) { return id == static_cast<int>(a) || id == static_cast<int>(c); };
    if (spec.quantity == Quantity::JointAngle || spec.quantity == Quantity::InteriorAngle)
        return BoneRefLabel(b.size() >= 2 ? b[1] : b[0]);
    if (spec.quantity == Quantity::Rotation) return BoneRefLabel(b[0]);
    if (spec.quantity == Quantity::Yaw) {
        if (b.size() >= 2 && side_of(b[0]) && side_of(b[0]) == side_of(b[1]) &&
            is_role(b[0], Role::LeftHeel, Role::RightHeel) && is_role(b[1], Role::LeftToe, Role::RightToe))
            return std::string(1, side_of(b[0])) + " foot";
        return BoneRefLabel(b[0]);
    }
    if (b.size() == 1) return BoneRefLabel(b[0]);
    // Several bones: "L heel+toe" when they are roles of one side, else "a+b".
    const char side = side_of(b[0]);
    bool       one_side = side != 0;
    for (int id : b)
        if (side_of(id) != side) one_side = false;
    std::string s;
    if (one_side) {
        s = std::string(1, side) + ' ';
        for (size_t i = 0; i < b.size(); ++i) s += (i ? "+" : "") + std::string(RolePart(static_cast<Role>(b[i])));
        return s;
    }
    for (size_t i = 0; i < b.size(); ++i) s += (i ? "+" : "") + BoneRefLabel(b[i]);
    return s;
}

std::string SignalName(const SignalSpec& spec, const std::string& bone_label)
{
    const char* measure = spec.measure == Measure::Speed ? "speed" : spec.measure == Measure::Acceleration ? "acceleration" : "";
    std::string word;
    if (spec.quantity == Quantity::JointAngle || spec.quantity == Quantity::Yaw) {
        word = spec.quantity == Quantity::JointAngle ? "bend" : "turn";
        if (*measure) word += std::string(" ") + measure;
    } else if (spec.quantity == Quantity::InteriorAngle) {
        word = "angle";
        if (*measure) word += std::string(" ") + measure;
    } else if (spec.quantity == Quantity::Rotation) {
        // "rotation X", "rotation X speed", "rotation speed" (total).
        word = "rotation";
        const bool total = spec.measure != Measure::Position &&
                           (spec.axis == Axis::Total || spec.axis == Axis::Horizontal);
        if (!total) {
            const Axis a = spec.axis;
            word += (a == Axis::Y || a == Axis::Vertical) ? " Y" : a == Axis::Z ? " Z" : " X";
        }
        if (*measure) word += std::string(" ") + measure;
    } else {
        std::string axis;
        switch (spec.axis) {
        case Axis::Vertical: axis = "vertical"; break;
        case Axis::Horizontal: axis = "horizontal"; break;
        case Axis::Total: axis = ""; break;
        case Axis::X: axis = "X"; break;
        case Axis::Y: axis = "Y"; break;
        case Axis::Z: axis = "Z"; break;
        }
        if (spec.measure == Measure::Position) {
            if (spec.axis == Axis::Vertical && spec.reference == Reference::Floor) word = "height";
            else if (spec.axis == Axis::Total && spec.reference == Reference::Bones) word = "distance";
            else word = axis.empty() ? "position" : axis + " position";
        } else {
            word = axis.empty() ? measure : axis + " " + measure;
        }
    }
    return bone_label.empty() ? word : bone_label + " " + word;
}

}  // namespace rav
