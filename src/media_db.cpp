// SPDX-License-Identifier: MIT
//
// See media_db.h. Story 13-5: database reader (SPIKE13 B), identity lookup (D20), background
// load, fallback folder index with its cache, favourites. No REAPER call, nothing throws.

#include "media_db.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <istream>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace rav {

namespace {

constexpr double kLengthTol = 0.0015;  // seconds

char LowerC(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string Lower(const std::string& s)
{
    std::string r = s;
    for (char& c : r) c = LowerC(c);
    return r;
}

// Lowercase, '\' -> '/', no trailing '/'.
std::string NormPath(const std::string& s)
{
    std::string r = Lower(s);
    for (char& c : r)
        if (c == '\\') c = '/';
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    return r;
}

std::vector<std::string> SplitPath(const std::string& p)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char c : p) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

std::string BaseName(const std::string& p)
{
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

bool EndsWithCI(const std::string& s, const std::string& suf)
{
    return s.size() >= suf.size() && Lower(s.substr(s.size() - suf.size())) == Lower(suf);
}

std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Number of equal trailing components (case-insensitive, '\' = '/').
int TrailingScore(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty()) return 0;
    const std::vector<std::string> pa = SplitPath(Lower(a)), pb = SplitPath(Lower(b));
    int n = 0;
    for (size_t i = pa.size(), j = pb.size(); i > 0 && j > 0; --i, --j) {
        if (pa[i - 1] != pb[j - 1]) break;
        ++n;
    }
    return n;
}

// Splits a database line into tokens. False on an unterminated quote.
bool Tokenize(const std::string& line, std::vector<std::string>* out)
{
    out->clear();
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= n) break;
        const char q = line[i];
        if (q == '"' || q == '\'' || q == '`') {
            const size_t e = line.find(q, i + 1);
            if (e == std::string::npos) return false;
            out->push_back(line.substr(i + 1, e - i - 1));
            i = e + 1;
        } else {
            size_t e = i;
            while (e < n && line[e] != ' ' && line[e] != '\t') ++e;
            out->push_back(line.substr(i, e - i));
            i = e;
        }
    }
    return true;
}

bool ParseInt64(const std::string& s, int64_t* v)
{
    if (s.empty()) return false;
    char* end = nullptr;
    const long long r = std::strtoll(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    *v = r;
    return true;
}

int ParseIntOr(const std::string& s, int dflt)
{
    int64_t v = 0;
    return ParseInt64(s, &v) ? static_cast<int>(v) : dflt;
}

bool ReadFileText(const std::string& path, std::string* text)
{
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *text = ss.str();
    return true;
}

bool WriteAtomic(const std::string& path, const std::string& text)
{
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) out << text;
        if (out) out.flush();
        const bool ok = static_cast<bool>(out);
        out.close();
        if (!ok || out.fail()) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string Fnv1a64Hex(const std::string& s)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

bool IsAudioExt(const std::string& name)
{
    static const char* const kExt[] = {".wav", ".aif", ".aiff", ".flac", ".mp3", ".ogg",
                                       ".opus", ".wv", ".w64", ".bwf", ".caf"};
    for (const char* e : kExt)
        if (EndsWithCI(name, e)) return true;
    return false;
}

bool IsAbsolutePath(const std::string& p)
{
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() >= 2 && p[1] == ':';
}

std::string JoinPath(const std::string& a, const std::string& b)
{
    return (fs::u8path(a) / fs::u8path(b)).u8string();
}

// The identity filter (name done by the caller): size, then length; unknown = wildcard.
bool IdentityMatch(const AssetIdentity& id, const DbFile& f)
{
    if (id.size >= 0 && f.size >= 0 && id.size != f.size) return false;
    if (id.length >= 0 && f.length >= 0 && std::fabs(id.length - f.length) > kLengthTol) return false;
    return true;
}

// Mutable parser state, shared by the string and stream entry points.
struct ParseState {
    bool    known_line = false;
    bool    have_file = false;
    size_t  unparsed = 0;
};

void ParseLine(std::string line, const std::string& db, std::vector<DbFile>* out, ParseState* st,
               std::vector<std::string>* tok)
{
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t") == std::string::npos) return;
    if (!Tokenize(line, tok) || tok->empty()) {
        // A broken FILE line: its DATA must not attach to the file before it.
        if (line.compare(line.find_first_not_of(" \t"), 4, "FILE") == 0) st->have_file = false;
        ++st->unparsed;
        return;
    }
    const std::string& kw = (*tok)[0];
    if (kw == "PATH" || kw == "USER") {
        st->known_line = true;
        return;
    }
    if (kw == "FILE") {
        if (tok->size() < 2 || (*tok)[1].empty()) {
            st->have_file = false;
            ++st->unparsed;
            return;
        }
        st->known_line = true;
        DbFile f;
        f.path = (*tok)[1];
        f.db = db;
        int64_t n1 = -1, n2 = 0, n3 = 0;
        if (tok->size() > 2 && ParseInt64((*tok)[2], &n1) && n1 >= 0) {
            if (tok->size() > 3 && ParseInt64((*tok)[3], &n2) && n2 > 0) n1 += n2 << 32;
            f.size = n1;
        }
        if (tok->size() > 4 && ParseInt64((*tok)[4], &n3)) f.mtime = n3;
        out->push_back(std::move(f));
        st->have_file = true;
        return;
    }
    if (kw == "DATA") {
        if (!st->have_file || out->empty()) {
            ++st->unparsed;
            return;
        }
        st->known_line = true;
        DbFile& f = out->back();
        for (size_t i = 1; i < tok->size(); ++i) {
            const std::string& t = (*tok)[i];
            const size_t c = t.find(':');
            if (c == std::string::npos || c == 0) continue;
            const std::string key = t.substr(0, c), v = t.substr(c + 1);
            if (key == "l") {
                double s = 0;
                if (ParseDbLength(v, &s)) f.length = s;
            } else if (key == "d") {
                f.description = v;
            } else if (key == "b") {
                f.library = v;
            } else if (key == "n") {
                f.channels = ParseIntOr(v, -1);
            } else if (key == "i") {
                f.bits = ParseIntOr(v, -1);
            } else if (key == "s") {
                f.rate = ParseIntOr(v, -1);
            } else if (!v.empty()) {
                // 13-69: every other field is kept as written, searched without being known.
                f.extra += key;
                f.extra += '\x1e';
                f.extra += v;
                f.extra += '\x1f';
            }
        }
        return;
    }
    ++st->unparsed;
}

}  // namespace

// ---------------------------------------------------------------- parsing

bool ParseDbLength(const std::string& v_in, double* seconds)
{
    const std::string v = Trim(v_in);
    if (v.empty() || !seconds) return false;
    std::vector<std::string> parts;
    size_t pos = 0;
    while (true) {
        const size_t c = v.find(':', pos);
        parts.push_back(v.substr(pos, c == std::string::npos ? std::string::npos : c - pos));
        if (c == std::string::npos) break;
        pos = c + 1;
    }
    if (parts.size() > 3) return false;
    double total = 0;
    for (const std::string& p : parts) {
        if (p.empty()) return false;
        for (char ch : p)
            if (!((ch >= '0' && ch <= '9') || ch == '.')) return false;
        char* end = nullptr;
        const double d = std::strtod(p.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(d)) return false;
        total = total * 60.0 + d;
    }
    *seconds = total;
    return true;
}

bool ParseReaperFileList(std::istream& in, const std::string& db, std::vector<DbFile>* out,
                         size_t* unparsed, const std::atomic<bool>* cancel)
{
    std::vector<DbFile> local;
    std::vector<DbFile>* dst = out ? out : &local;
    ParseState st;
    std::vector<std::string> tok;
    std::string line;
    bool any_text = false;
    size_t n = 0;
    while (std::getline(in, line)) {
        if (cancel && (++n & 4095) == 0 && cancel->load()) {
            if (unparsed) *unparsed = st.unparsed;
            return false;
        }
        if (!any_text && line.find_first_not_of(" \t\r") != std::string::npos) any_text = true;
        ParseLine(line, db, dst, &st, &tok);
    }
    if (unparsed) *unparsed = st.unparsed;
    return !any_text || st.known_line;
}

bool ParseReaperFileList(const std::string& text, const std::string& db, std::vector<DbFile>* out,
                         size_t* unparsed)
{
    std::istringstream in(text);
    return ParseReaperFileList(in, db, out, unparsed, nullptr);
}

std::vector<DbEntry> ListMediaDatabases(const std::string& resource_path, const std::string& ini_text)
{
    // section name (lowercase) -> key -> value
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> sections;
    std::string cur;
    std::istringstream in(ini_text);
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            const size_t e = line.find(']');
            cur = Lower(line.substr(1, e == std::string::npos ? std::string::npos : e - 1));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || cur.empty()) continue;
        sections[cur].emplace_back(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
    }
    auto it = sections.find("reaper_sexplorer");
    if (it == sections.end()) it = sections.find("reaper_explorer");
    if (it == sections.end()) return {};
    std::map<long, std::string> paths, titles;
    for (const auto& kv : it->second) {
        const std::string k = Lower(kv.first);
        if (k.size() <= 8 || k.compare(0, 8, "shortcut") != 0) continue;  // short keys: no substr throw
        const bool title = k[8] == 't';
        const std::string num = k.substr(title ? 9 : 8);
        if (num.empty() ||
            num.find_first_not_of("0123456789") != std::string::npos)
            continue;
        const long n = std::strtol(num.c_str(), nullptr, 10);
        (title ? titles : paths)[n] = kv.second;
    }
    std::vector<DbEntry> out;
    std::unordered_set<std::string> seen;
    for (const auto& kv : paths) {
        const std::string& v = kv.second;
        if (!EndsWithCI(v, ".ReaperFileList")) continue;
        DbEntry e;
        e.path = IsAbsolutePath(v) ? v : JoinPath(JoinPath(resource_path, "MediaDB"), v);
        if (!seen.insert(NormPath(e.path)).second) continue;
        const auto t = titles.find(kv.first);
        if (t != titles.end() && !t->second.empty()) {
            e.name = t->second;
        } else {
            const std::string b = BaseName(v);
            e.name = b.substr(0, b.size() - std::string(".ReaperFileList").size());
        }
        out.push_back(std::move(e));
    }
    return out;
}

// ---------------------------------------------------------------- index

void MediaDbIndex::Add(const DbFile& f)
{
    Add(DbFile(f));
}

void MediaDbIndex::Add(DbFile&& f)
{
    const uint32_t i = static_cast<uint32_t>(files_.size());
    const std::string name = BaseName(f.path);
    by_name_[Lower(name)].push_back(i);
    // 13-69: one lowercase text per file: name, folders (no drive), then every DATA field, each
    // ended by \x1f (a query word never spans two of them; words hold no control character).
    std::string h;
    h.reserve(f.path.size() + f.description.size() + f.library.size() + f.extra.size() + 12);
    h += name;
    const uint32_t name_end = static_cast<uint32_t>(h.size());
    h += '\x1f';
    size_t from = 0;
    if (f.path.size() >= 2 && f.path[1] == ':') from = 2;  // "C:" is not a folder
    const size_t base = f.path.size() - name.size();
    while (from < base && (f.path[from] == '\\' || f.path[from] == '/')) ++from;
    if (from < base) h.append(f.path, from, base - from);
    h += '\x1f';
    if (!f.description.empty()) h += std::string("d\x1e") + f.description + '\x1f';
    if (!f.library.empty()) h += std::string("b\x1e") + f.library + '\x1f';
    h += f.extra;
    for (char& c : h) c = LowerC(c);
    h.shrink_to_fit();
    hay_.push_back(std::move(h));
    name_end_.push_back(name_end);
    f.extra.clear();
    f.extra.shrink_to_fit();
    files_.push_back(std::move(f));
}

void MediaDbIndex::Clear()
{
    files_.clear();
    by_name_.clear();
    hay_.clear();
    name_end_.clear();
}

size_t MediaDbIndex::Size() const
{
    return files_.size();
}

LookupResult MediaDbIndex::Find(const AssetIdentity& id) const
{
    LookupResult r;
    if (id.name.empty()) return r;
    const auto it = by_name_.find(Lower(BaseName(id.name)));
    if (it == by_name_.end()) return r;
    std::vector<uint32_t> cand;
    std::unordered_set<std::string> seen;  // one file listed by two databases is one candidate
    for (uint32_t i : it->second)
        if (IdentityMatch(id, files_[i]) && seen.insert(NormPath(files_[i].path)).second) cand.push_back(i);
    if (cand.empty()) return r;
    if (cand.size() == 1) {
        r.status = LookupStatus::Found;
        r.path = files_[cand[0]].path;
        return r;
    }
    int best = -1, best_n = 0;
    uint32_t best_i = 0;
    for (uint32_t i : cand) {
        const int s = std::max(TrailingScore(files_[i].path, id.last_path), TrailingScore(files_[i].path, id.rel_path));
        if (s > best) {
            best = s;
            best_n = 1;
            best_i = i;
        } else if (s == best) {
            ++best_n;
        }
    }
    if (best >= 2 && best_n == 1) {
        r.status = LookupStatus::Found;
        r.path = files_[best_i].path;
        return r;
    }
    r.status = LookupStatus::Ambiguous;
    for (uint32_t i : cand) r.candidates.push_back(files_[i].path);
    return r;
}

namespace {

std::vector<std::string> QueryWords(const std::string& query)
{
    std::vector<std::string> words;
    std::istringstream in(Lower(query));
    std::string w;
    while (in >> w) words.push_back(w);
    return words;
}

// REAPER's one-letter DATA keys, as the Media Explorer names its columns; others as written.
std::string FieldLabel(const std::string& key)
{
    static const char* const kNames[][2] = {{"d", "description"}, {"b", "library"}, {"t", "title"},
                                            {"a", "artist"},      {"g", "genre"},   {"c", "comment"},
                                            {"y", "date"},        {"k", "key"},     {"p", "BPM"}};
    for (const auto& n : kNames)
        if (key == n[0]) return n[1];
    return key;
}

// 13-69 review: the first hit of `w` outside the field keys ("key\x1evalue\x1f"): a word must
// match what a file holds, not the name of a field it has ("d", "b", a custom key).
size_t FindInValues(const std::string& h, const std::string& w)
{
    for (size_t at = h.find(w); at != std::string::npos; at = h.find(w, at + 1)) {
        const size_t sep = h.find_first_of("\x1e\x1f", at + w.size());
        if (sep == std::string::npos || h[sep] != '\x1e') return at;
    }
    return std::string::npos;
}

}  // namespace

std::vector<const DbFile*> MediaDbIndex::Search(const std::string& query, const std::string& db_filter,
                                                size_t max) const
{
    const std::vector<std::string> words = QueryWords(query);
    // 13-69: name hits first; the scan stops once `max` name hits are found.
    std::vector<const DbFile*> named, other;
    for (size_t i = 0; i < files_.size() && named.size() < max; ++i) {
        if (!db_filter.empty() && files_[i].db != db_filter) continue;
        const std::string& h = hay_[i];
        bool ok = true, in_name = true;
        for (const std::string& w : words) {
            const size_t at = FindInValues(h, w);
            if (at == std::string::npos) {
                ok = false;
                break;
            }
            if (at + w.size() > name_end_[i]) in_name = false;  // the first hit is past the name
        }
        if (!ok) continue;
        if (in_name) named.push_back(&files_[i]);
        else if (other.size() < max) other.push_back(&files_[i]);
    }
    for (const DbFile* f : other) {
        if (named.size() >= max) break;
        named.push_back(f);
    }
    return named;
}

std::vector<std::string> MediaDbIndex::MatchedFields(const DbFile* f, const std::string& query) const
{
    std::vector<std::string> out;
    if (!f || files_.empty() || f < files_.data() || f >= files_.data() + files_.size()) return out;
    const size_t i = static_cast<size_t>(f - files_.data());
    const std::vector<std::string> words = QueryWords(query);
    if (words.empty()) return out;
    const std::string& h = hay_[i];
    auto add = [&](const std::string& label) {
        if (std::find(out.begin(), out.end(), label) == out.end()) out.push_back(label);
    };
    // The fields, each [b, e): name, folders, then key\x1evalue.
    size_t b = 0;
    int    n = 0;
    while (b <= h.size()) {
        size_t e = h.find('\x1f', b);
        if (e == std::string::npos) e = h.size();
        if (e == b && n >= 2) break;
        std::string label;
        size_t      vb = b;
        if (n == 0) {
            label = "name";
        } else if (n == 1) {
            label = "folder";
        } else {
            const size_t k = h.find('\x1e', b);
            if (k == std::string::npos || k > e) {
                vb = b;
                label = "data";
            } else {
                vb = k + 1;
                label = FieldLabel(h.substr(b, k - b));
            }
        }
        const std::string_view v(h.data() + vb, e - vb);
        for (const std::string& w : words)
            if (v.find(w) != std::string_view::npos) {
                add(label);
                break;
            }
        ++n;
        if (e >= h.size()) break;
        b = e + 1;
    }
    return out;
}

size_t MediaDbIndex::SearchTextBytes() const
{
    size_t n = 0;
    for (const std::string& h : hay_) n += h.capacity();
    return n;
}

LookupResult LocateFolder(const AssetIdentity& id, const std::string& folder,
                          const std::function<std::vector<DbFile>(const std::string&)>& list_folder)
{
    if (!list_folder) return {};
    MediaDbIndex idx;
    for (const DbFile& f : list_folder(folder)) idx.Add(f);
    return idx.Find(id);
}

std::vector<LookupResult> RelocateFolder(const AssetIdentity& located, const std::string& located_path,
                                         const std::vector<AssetIdentity>& missing,
                                         const std::function<std::vector<DbFile>(const std::string&)>& list_folder)
{
    std::vector<LookupResult> out(missing.size());
    const std::vector<std::string> po = SplitPath(located.last_path), pn = SplitPath(located_path);
    const int k = TrailingScore(located.last_path, located_path);
    if (k < 1 || !list_folder) return out;
    // Old / new prefixes: the components before the common tail.
    std::vector<std::string> old_pre(po.begin(), po.end() - k);
    std::string new_pre;
    {
        const std::string np = located_path;
        // Keep the new path's own spelling: cut its last k components.
        size_t cut = np.size();
        for (int i = 0; i < k; ++i) {
            while (cut > 0 && (np[cut - 1] == '/' || np[cut - 1] == '\\')) --cut;
            while (cut > 0 && np[cut - 1] != '/' && np[cut - 1] != '\\') --cut;
        }
        new_pre = np.substr(0, cut);
        while (!new_pre.empty() && (new_pre.back() == '/' || new_pre.back() == '\\')) new_pre.pop_back();
    }
    std::map<std::string, MediaDbIndex> folders;  // normalized folder -> its files
    for (size_t m = 0; m < missing.size(); ++m) {
        const std::vector<std::string> pm = SplitPath(missing[m].last_path);
        if (pm.size() <= old_pre.size()) continue;
        bool under = true;
        for (size_t i = 0; i < old_pre.size(); ++i)
            if (Lower(pm[i]) != Lower(old_pre[i])) {
                under = false;
                break;
            }
        if (!under) continue;
        std::string dir = new_pre;
        for (size_t i = old_pre.size(); i + 1 < pm.size(); ++i) dir += (dir.empty() ? "" : "/") + pm[i];
        const std::string key = NormPath(dir);
        auto it = folders.find(key);
        if (it == folders.end()) {
            it = folders.emplace(key, MediaDbIndex()).first;
            for (const DbFile& f : list_folder(dir)) it->second.Add(f);
        }
        AssetIdentity id = missing[m];
        id.last_path = dir + "/" + pm.back();
        out[m] = it->second.Find(id);
    }
    return out;
}

namespace {

// `root` joined with `rel` (components joined by '/'), keeping root's own spelling.
std::string JoinUnder(const std::string& root, const std::string& rel)
{
    if (rel.empty()) return root;
    if (root.empty()) return rel;
    const char last = root.back();
    return (last == '/' || last == '\\') ? root + rel : root + "/" + rel;
}

// A component that names a drive ("D:"), never a folder under the picked root.
bool IsDriveComponent(const std::string& c)
{
    return c.find(':') != std::string::npos;
}

}  // namespace

std::vector<LookupResult> LocateUnderRoot(const std::string& root, const std::vector<AssetIdentity>& missing,
                                          const std::function<std::vector<DbFile>(const std::string&)>& list_folder,
                                          const std::atomic<bool>* cancel)
{
    std::vector<LookupResult> out(missing.size());
    if (root.empty() || !list_folder) return out;
    auto cancelled = [cancel] { return cancel && cancel->load(); };
    // Phase 1: the targeted folders (the tail of each old folder under root), each listed once.
    std::map<std::string, MediaDbIndex> folders;  // normalized folder -> its files
    for (size_t m = 0; m < missing.size(); ++m) {
        if (cancelled()) return out;
        const std::vector<std::string> pm = SplitPath(missing[m].last_path);
        if (pm.size() < 2) continue;  // no folder part
        const size_t nf = pm.size() - 1;  // folder components
        for (size_t k = nf; k >= 1; --k) {
            bool drive = false;
            std::string rel;
            for (size_t i = nf - k; i < nf; ++i) {
                if (IsDriveComponent(pm[i])) drive = true;
                rel += (rel.empty() ? "" : "/") + pm[i];
            }
            if (drive) continue;
            const std::string dir = JoinUnder(root, rel);
            const std::string key = NormPath(dir);
            auto it = folders.find(key);
            if (it == folders.end()) {
                it = folders.emplace(key, MediaDbIndex()).first;
                for (const DbFile& f : list_folder(dir)) it->second.Add(f);
                if (cancelled()) return out;
            }
            if (it->second.Size() == 0) continue;
            const LookupResult r = it->second.Find(missing[m]);
            if (r.status == LookupStatus::Found) {
                out[m] = r;
                break;
            }
        }
    }
    // Phase 2: the whole root, for those still not Found.
    bool rest = false;
    for (const LookupResult& r : out)
        if (r.status != LookupStatus::Found) rest = true;
    if (!rest || cancelled()) return out;
    MediaDbIndex all;
    for (const DbFile& f : list_folder(root)) all.Add(f);
    if (cancelled()) return out;
    for (size_t m = 0; m < missing.size(); ++m)
        if (out[m].status != LookupStatus::Found) out[m] = all.Find(missing[m]);
    return out;
}

LocateOutcome ClassifyLocate(const std::vector<AssetIdentity>& missing, const std::vector<LookupResult>& res,
                             const AssetIdentity* self, const std::string& self_path)
{
    LocateOutcome out;
    if (self) out.found.push_back({*self, self_path});
    for (size_t i = 0; i < res.size() && i < missing.size(); ++i) {
        if (res[i].status == LookupStatus::Found && !res[i].path.empty()) out.found.push_back({missing[i], res[i].path});
        else if (res[i].status == LookupStatus::Ambiguous) ++out.ambiguous;
    }
    return out;
}

std::string LocateToastText(int changed, int missing, int ambiguous)
{
    const int   m = std::max(missing, changed);
    std::string msg = "Located " + std::to_string(changed) + " of " + std::to_string(m) + " sounds";
    if (ambiguous > 0) msg += ", " + std::to_string(ambiguous) + " ambiguous";
    return msg;
}

AssetIdentity RenamedLocateKey(const std::string& stored_path, const AssetIdentity& located)
{
    AssetIdentity key = located;
    size_t        cut = stored_path.size();
    while (cut > 0 && (stored_path[cut - 1] == '/' || stored_path[cut - 1] == '\\')) --cut;
    while (cut > 0 && stored_path[cut - 1] != '/' && stored_path[cut - 1] != '\\') --cut;
    key.last_path = stored_path.substr(0, cut) + located.name;  // keeps the old folder's separator
    return key;
}

std::string RelinkToastText(const std::string& old_name, const std::string& new_name, int others_changed,
                            int others_missing, int ambiguous)
{
    std::string msg = "Relinked " + old_name + " under a new name: " + new_name;
    const int   m = std::max(others_missing, others_changed);
    if (m > 0) {
        msg += "; located " + std::to_string(others_changed) + " of " + std::to_string(m) + " other sounds";
        if (ambiguous > 0) msg += ", " + std::to_string(ambiguous) + " ambiguous";
    }
    return msg;
}

// ---------------------------------------------------------------- folder index (D7)

std::vector<DbFile> ScanFolder(const std::string& folder, const std::vector<DbFile>& cached,
                               const std::function<double(const std::string&)>& length_of, size_t* calls,
                               const std::atomic<bool>* cancel)
{
    if (calls) *calls = 0;
    std::unordered_map<std::string, const DbFile*> cache;
    for (const DbFile& f : cached) cache[NormPath(f.path)] = &f;
    std::vector<DbFile> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(fs::u8path(folder), fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (cancel && cancel->load()) break;
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;
        const std::string path = it->path().u8string();
        if (!IsAudioExt(path)) continue;
        DbFile f;
        f.path = path;
        f.db = folder;
        const auto sz = fs::file_size(it->path(), e2);
        f.size = e2 ? -1 : static_cast<int64_t>(sz);
        const auto t = fs::last_write_time(it->path(), e2);
        f.mtime = e2 ? 0 : static_cast<int64_t>(
                               std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
        const auto c = cache.find(NormPath(path));
        if (c != cache.end() && c->second->size == f.size && c->second->mtime == f.mtime &&
            c->second->length >= 0) {
            f.length = c->second->length;
        } else if (length_of) {
            f.length = length_of(path);
            if (calls) ++*calls;
        }
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const DbFile& a, const DbFile& b) { return a.path < b.path; });
    return out;
}

std::string MediaCacheDir(const std::string& resource_path)
{
    return (fs::u8path(resource_path) / "ReaAnimViewer" / "mediadb").u8string();
}

std::string FolderCachePath(const std::string& resource_path, const std::string& folder)
{
    return (fs::u8path(MediaCacheDir(resource_path)) / (Fnv1a64Hex(NormPath(folder)) + ".ReaperFileList")).u8string();
}

std::string FolderCacheText(const std::vector<DbFile>& files)
{
    std::string s;
    char buf[160];
    for (const DbFile& f : files) {
        const char q = f.path.find('"') == std::string::npos ? '"' : '`';
        const int64_t size = f.size < 0 ? 0 : f.size;
        std::snprintf(buf, sizeof(buf), " %llu %llu %lld 0\n",
                      static_cast<unsigned long long>(size & 0xffffffffLL),
                      static_cast<unsigned long long>(size >> 32), static_cast<long long>(f.mtime));
        s += "FILE ";
        s += q;
        s += f.path;
        s += q;
        s += buf;
        if (f.length >= 0) {
            std::snprintf(buf, sizeof(buf), "DATA \"l:%.6f\"\n", f.length);
            s += buf;
        }
    }
    return s;
}

bool LoadFolderCache(const std::string& cache_path, std::vector<DbFile>* out)
{
    std::string text;
    if (!out || !ReadFileText(cache_path, &text)) return false;
    return ParseReaperFileList(text, "", out, nullptr);
}

bool SaveFolderCache(const std::string& cache_path, const std::vector<DbFile>& files)
{
    return WriteAtomic(cache_path, FolderCacheText(files));
}

std::function<std::vector<DbFile>(const std::string&)> MakeCachedFolderLister(
    const std::string& resource_path, std::function<double(const std::string&)> length_of,
    const std::atomic<bool>* cancel)
{
    return [resource_path, length_of, cancel](const std::string& folder) {
        std::error_code ec;
        if (folder.empty() || !fs::is_directory(fs::u8path(folder), ec) || ec) return std::vector<DbFile>();
        const std::string cp = FolderCachePath(resource_path, folder);
        std::vector<DbFile> cached;
        std::string old;
        const bool had = ReadFileText(cp, &old);
        if (had) ParseReaperFileList(old, "", &cached, nullptr);
        std::vector<DbFile> files = ScanFolder(folder, cached, length_of, nullptr, cancel);
        if (cancel && cancel->load()) return files;  // partial: never cached
        if (!had || old != FolderCacheText(files)) SaveFolderCache(cp, files);
        return files;
    };
}

// ---------------------------------------------------------------- background load (D5)

namespace {

struct LoadState {
    std::mutex                          m;  // guards idx, info, thread
    std::shared_ptr<const MediaDbIndex> idx = std::make_shared<MediaDbIndex>();
    MediaDbInfo                         info;
    std::thread                         th;
    std::atomic<bool>                   cancel{false};
    std::atomic<bool>                   ready{false};
};

// Never destroyed: a load still running at exit is never terminate()d.
LoadState& State()
{
    static LoadState* s = new LoadState();
    return *s;
}

void LoadWorkerBody(const std::string& resource_path)
{
    LoadState& s = State();
    const auto t0 = std::chrono::steady_clock::now();
    std::string ini;
    ReadFileText(JoinPath(resource_path, "reaper.ini"), &ini);
    std::vector<DbEntry> dbs = ListMediaDatabases(resource_path, ini);
    if (dbs.empty()) {
        std::vector<std::string> names;
        std::error_code ec;
        for (fs::directory_iterator it(fs::u8path(JoinPath(resource_path, "MediaDB")), ec), end; !ec && it != end;
             it.increment(ec)) {
            const std::string n = it->path().filename().u8string();
            if (n.compare(0, 2, "||") == 0 || !EndsWithCI(n, ".ReaperFileList")) continue;
            names.push_back(n);
        }
        std::sort(names.begin(), names.end());
        for (const std::string& n : names)
            dbs.push_back({n.substr(0, n.size() - 15), JoinPath(JoinPath(resource_path, "MediaDB"), n)});
    }
    auto idx = std::make_shared<MediaDbIndex>();
    MediaDbInfo info;
    std::vector<DbFile> files;
    for (const DbEntry& e : dbs) {
        if (s.cancel.load()) return;
        std::ifstream in(fs::u8path(e.path), std::ios::binary);
        if (!in) continue;
        files.clear();
        size_t bad = 0;
        ParseReaperFileList(in, e.name, &files, &bad, &s.cancel);
        if (s.cancel.load()) return;
        size_t added = 0;
        for (DbFile& f : files) {
            if ((++added & 4095) == 0 && s.cancel.load()) return;
            idx->Add(std::move(f));  // 13-69: no copy of the search fields
        }
        info.unparsed += bad;
        info.databases.push_back(e);
    }
    info.files = idx->Size();
    info.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard<std::mutex> lk(s.m);
    if (s.cancel.load()) return;
    s.idx = std::move(idx);
    s.info = std::move(info);
    s.ready = true;
}

// Thread entry: an exception escaping a std::thread would terminate REAPER.
void LoadWorker(std::string resource_path)
{
    try {
        LoadWorkerBody(resource_path);
    } catch (...) {
        // the previous index stays; not ready
    }
}

}  // namespace

void MediaDbStop()
{
    LoadState& s = State();
    s.cancel = true;
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(s.m);
        th = std::move(s.th);
    }
    if (th.joinable()) th.join();
}

void MediaDbStartLoad(const std::string& resource_path)
{
    MediaDbStop();
    LoadState& s = State();
    std::lock_guard<std::mutex> lk(s.m);
    s.cancel = false;
    s.ready = false;
    try {
        s.th = std::thread(LoadWorker, resource_path);
    } catch (...) {
        // no thread: the previous index stays, never ready
    }
}

bool MediaDbReady()
{
    return State().ready.load();
}

std::shared_ptr<const MediaDbIndex> MediaDbGet()
{
    LoadState& s = State();
    std::lock_guard<std::mutex> lk(s.m);
    return s.idx;
}

MediaDbInfo MediaDbGetInfo()
{
    LoadState& s = State();
    std::lock_guard<std::mutex> lk(s.m);
    return s.info;
}

// ---------------------------------------------------------------- favourites (D8)

std::vector<std::string> LoadMediaFavourites(const std::string& text)
{
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        if (seen.insert(NormPath(line)).second) out.push_back(line);
    }
    return out;
}

std::string SaveMediaFavourites(const std::vector<std::string>& paths)
{
    std::string s;
    for (const std::string& p : LoadMediaFavourites([&] {
             std::string all;
             for (const std::string& q : paths) all += q + "\n";
             return all;
         }()))
        s += p + "\n";
    return s;
}

std::string MediaFavouritesPath(const std::string& resource_path)
{
    return (fs::u8path(resource_path) / "ReaAnimViewer" / "media_favourites.txt").u8string();
}

std::vector<std::string> ReadMediaFavouritesFile(const std::string& resource_path)
{
    std::string text;
    if (!ReadFileText(MediaFavouritesPath(resource_path), &text)) return {};
    return LoadMediaFavourites(text);
}

bool WriteMediaFavouritesFile(const std::string& resource_path, const std::vector<std::string>& paths)
{
    return WriteAtomic(MediaFavouritesPath(resource_path), SaveMediaFavourites(paths));
}

}  // namespace rav
