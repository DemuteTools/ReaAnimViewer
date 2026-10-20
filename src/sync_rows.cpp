// SPDX-License-Identifier: MIT
//
// See sync_rows.h (story 13-77c, readability pass).

#include "sync_rows.h"

#include "library_sync_select.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <unordered_map>

namespace rav {

namespace {

const char kMinus[] = "\xE2\x88\x92";  // U+2212

std::string KindOfKey(const std::string& key)
{
    return key.substr(0, key.find('|'));
}

bool Starts(const std::string& s, const char* p)
{
    return s.compare(0, std::char_traits<char>::length(p), p) == 0;
}

bool Ends(const std::string& s, const char* p)
{
    const size_t n = std::char_traits<char>::length(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

// "... (3 sounds)" / "(1 sound)" at the end of a removal's text.
int SoundsInText(const std::string& t)
{
    const size_t open = t.rfind('(');
    if (open == std::string::npos || (!Ends(t, " sounds)") && !Ends(t, " sound)"))) return 0;
    return std::atoi(t.c_str() + open + 1);
}

std::string Plural(int n, const char* one, const char* many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// A variation change: 0 edited, 1 added by the side that moved, 2 removed by it.
int VariationKind(const SyncChange& c)
{
    if (c.side == SyncSide::Here) return c.here_text == "absent" ? 2 : c.file_text == "absent" ? 1 : 0;
    if (c.side == SyncSide::File) return c.file_text == "absent" ? 2 : c.here_text == "absent" ? 1 : 0;
    return 0;
}

const char* WhereOf(SyncSide s)
{
    return s == SyncSide::Here ? "here" : s == SyncSide::File ? "in the global" : "on both sides";
}

}  // namespace

std::string SyncCaseValueName(const std::string& label)
{
    const std::string pre = "Switch value ";
    return Starts(label, pre.c_str()) ? label.substr(pre.size()) : label;
}

std::vector<SyncRowGroup> SyncGroupChanges(const SyncDiff& d)
{
    const size_t                            n = d.changes.size();
    std::unordered_map<std::string, size_t> at;
    for (size_t i = 0; i < n; ++i) at[d.changes[i].key] = i;
    std::vector<int>          gid(n, -1);
    std::vector<SyncRowGroup> groups;
    std::vector<size_t>       lead_at;  // the diff position of each group's lead (the order of the groups)

    auto side_of = [&](const std::vector<std::string>& keys) {
        SyncSide s = d.changes[at[keys[0]]].side;
        for (const std::string& k : keys)
            if (d.changes[at[k]].side != s) return SyncSide::Both;
        return s;
    };

    // pass 1: added / removed branches (a lead has no X it needs: nothing of the same side above it)
    for (size_t i = 0; i < n; ++i) {
        const SyncChange& c = d.changes[i];
        if (gid[i] >= 0 || c.key.compare(0, 2, "X|") != 0 || c.side == SyncSide::Both) continue;
        SyncSide adder = SyncSide::Both;
        const bool added = SyncIsAddition(c, &adder);
        const bool removed = !added && ((c.side == SyncSide::Here && c.here_text == "removed") || (c.side == SyncSide::File && c.file_text == "removed"));
        if (!added && !removed) continue;
        bool has_x_need = false;
        for (const std::string& k : c.needs)
            if (k.compare(0, 2, "X|") == 0) has_x_need = true;
        if (has_x_need) continue;
        std::set<std::string> in;
        in.insert(c.key);
        for (const std::string& k : DependentsClosure(d, c.key)) {
            const auto it = at.find(k);
            if (it != at.end() && gid[it->second] < 0 && d.changes[it->second].side == c.side) in.insert(k);
        }
        // the Switch value / group only this branch uses goes with it
        for (bool grew = true; grew;) {
            grew = false;
            const std::set<std::string> now = in;
            for (const std::string& k : now)
                for (const std::string& nk : d.changes[at[k]].needs) {
                    if (in.count(nk) || (nk.compare(0, 3, "VX|") != 0 && nk.compare(0, 3, "GX|") != 0)) continue;
                    const auto it = at.find(nk);
                    if (it == at.end() || gid[it->second] >= 0 || d.changes[it->second].side != c.side) continue;
                    bool only_here = true;
                    for (const std::string& dk : DependentsClosure(d, nk))
                        if (!in.count(dk)) only_here = false;
                    if (only_here) {
                        in.insert(nk);
                        grew = true;
                    }
                }
        }
        if (in.size() < 2) continue;
        SyncRowGroup g;
        g.kind = added ? SyncRowKind::Added : SyncRowKind::Removed;
        g.keys.push_back(c.key);
        for (const SyncChange& o : d.changes)
            if (o.key != c.key && in.count(o.key)) g.keys.push_back(o.key);
        g.side = c.side;
        if (removed) g.sounds = c.adds_container ? SoundsInText(c.text) : 1;
        for (const std::string& k : g.keys) {
            const SyncChange& m = d.changes[at[k]];
            if (added && Starts(k, "X|") && !m.adds_container) ++g.sounds;
            if (g.value.empty() && Starts(k, "C|") && std::find(m.needs.begin(), m.needs.end(), c.key) != m.needs.end())
                g.value = SyncCaseValueName(m.label);
            gid[at[k]] = static_cast<int>(groups.size());
        }
        lead_at.push_back(i);
        groups.push_back(std::move(g));
    }

    // pass 2: one row per Sound for its variation / part changes
    std::unordered_map<std::string, size_t> by_sound;
    for (size_t i = 0; i < n; ++i) {
        const SyncChange& c = d.changes[i];
        const std::string kind = KindOfKey(c.key);
        if (gid[i] >= 0 || (kind != "V" && kind != "P" && kind != "PK")) continue;
        auto it = by_sound.find(c.subject);
        if (it == by_sound.end()) {
            SyncRowGroup g;
            g.kind = SyncRowKind::Edited;
            it = by_sound.emplace(c.subject, groups.size()).first;
            lead_at.push_back(i);
            groups.push_back(std::move(g));
        }
        SyncRowGroup& g = groups[it->second];
        g.keys.push_back(c.key);
        gid[i] = static_cast<int>(it->second);
        if (kind == "V") {
            const int vk = VariationKind(c);
            (vk == 1 ? g.added : vk == 2 ? g.removed : g.edited)++;
        } else if (kind == "P") ++g.parts;
        else ++g.parked;
    }
    for (SyncRowGroup& g : groups)
        if (g.kind == SyncRowKind::Edited) g.side = side_of(g.keys);

    // pass 3: everything else is its own row
    for (size_t i = 0; i < n; ++i) {
        if (gid[i] >= 0) continue;
        SyncRowGroup g;
        g.keys.push_back(d.changes[i].key);
        g.side = d.changes[i].side;
        gid[i] = static_cast<int>(groups.size());
        lead_at.push_back(i);
        groups.push_back(std::move(g));
    }

    std::vector<size_t> order(groups.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return lead_at[a] < lead_at[b]; });
    std::vector<SyncRowGroup> out;
    out.reserve(groups.size());
    for (size_t i : order) out.push_back(std::move(groups[i]));
    return out;
}

std::string SyncNodeDisplayName(const SyncDiff& d, const std::string& id)
{
    const std::string g = SyncGhostName(d, id);
    if (!g.empty() && g != id) return g;
    const std::string xk = "X|" + id;
    for (const SyncChange& c : d.changes)
        if (c.key.compare(0, 2, "C|") == 0 && std::find(c.needs.begin(), c.needs.end(), xk) != c.needs.end()) return SyncCaseValueName(c.label);
    return SyncGhostType(d, id);
}

std::string SyncEditSummary(const SyncRowGroup& g)
{
    std::vector<std::string> parts;
    const bool               vars = g.edited + g.added + g.removed > 0;
    if (g.edited > 0) parts.push_back(Plural(g.edited, "variation", "variations") + " edited");
    if (g.added > 0) parts.push_back(g.edited > 0 ? std::to_string(g.added) + " added" : Plural(g.added, "variation", "variations") + " added");
    if (g.removed > 0)
        parts.push_back(g.edited + g.added > 0 ? std::to_string(g.removed) + " removed" : Plural(g.removed, "variation", "variations") + " removed");
    (void)vars;
    if (g.parts > 0) parts.push_back("parts edited");
    if (g.parked > 0) parts.push_back("kept variations edited");
    if (parts.empty()) return "edited";
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i) s += (i ? ", " : "") + parts[i];
    return s;
}

std::string SyncEditLine(const SyncChange& c)
{
    const std::string kind = KindOfKey(c.key);
    std::string       what = "edited";
    if (kind == "V") {
        const int vk = VariationKind(c);
        what = vk == 1 ? "added" : vk == 2 ? "removed" : "edited";
    }
    const std::string name = c.label.empty() ? c.text : c.label;
    return name + ": " + what + " " + WhereOf(c.side);
}

std::string SyncBranchCell(const SyncRowGroup& g, const std::string& name)
{
    std::string s = g.kind == SyncRowKind::Removed ? std::string("\xE2\x88\x92 ") : std::string("+ ");
    if (!g.value.empty()) s += g.value + " \xC2\xB7 plays " + name;
    else s += name;
    if (g.sounds >= 2) s += " (" + std::to_string(g.sounds) + " sounds)";
    return s;
}

std::string SyncSettingName(const std::string& label)
{
    if (label == "Offset") return "Snap offset";
    std::string s = label;
    if (Ends(s, " changed")) s.erase(s.size() - 8);
    for (char& ch : s)
        if (ch == '_') ch = ' ';
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s.empty() ? label : s;
}

namespace {

std::string Minus(const std::string& s)
{
    if (s.size() >= 2 && s[0] == '-' && std::isdigit(static_cast<unsigned char>(s[1]))) return kMinus + s.substr(1);
    return s;
}

}  // namespace

std::string SyncSettingValue(const std::string& label, const std::string& text)
{
    if (text.empty()) return "not set";
    if (Ends(label, " changed")) {  // a setting the engine has no word for: its raw flag
        if (text == "1") return "yes";
        if (text == "0") return "no";
        return text;
    }
    std::string t = text;
    if (label == "Fades") {
        const bool word = std::isalpha(static_cast<unsigned char>(t[0])) != 0;
        if (word) {  // "default ms"
            if (Ends(t, " ms")) t.erase(t.size() - 3);
            return t;
        }
        std::string unit;
        if (Ends(t, " ms")) {
            unit = " ms";
            t.erase(t.size() - 3);
        }
        const size_t sl = t.find('/');
        const std::string a = t.substr(0, sl), b = sl == std::string::npos ? std::string("-") : t.substr(sl + 1);
        auto one = [&](const std::string& v) { return v == "-" || v.empty() ? std::string("none") : Minus(v) + unit; };
        return "in " + one(a) + " \xC2\xB7 out " + one(b);
    }
    if (std::isalpha(static_cast<unsigned char>(t[0]))) {  // "default dB": a word with its unit left on
        if (Ends(t, " dB")) t.erase(t.size() - 3);
        else if (Ends(t, " ms")) t.erase(t.size() - 3);
        return t;
    }
    return Minus(t);
}

std::vector<std::pair<int, std::string>> SyncBranchLines(const SyncDiff& d, const SyncRowGroup& g)
{
    std::vector<std::pair<int, std::string>> out;
    if (g.keys.empty()) return out;
    const SyncChange* lead = d.Find(g.keys[0]);
    if (!lead) return out;
    bool has_value_line = false;
    for (const std::string& k : g.keys)
        if (Starts(k, "VX|") || Starts(k, "GX|")) has_value_line = true;
    for (const std::string& k : g.keys) {
        const SyncChange* c = d.Find(k);
        if (!c) continue;
        if (Starts(k, "X|")) {
            std::string name = c->label;
            if (name.empty() || name == c->subject) name = SyncNodeDisplayName(d, c->subject);
            const size_t sp = c->text.find(' ');
            const std::string word = c->adds_container && sp != std::string::npos ? c->text.substr(0, sp) + " " : std::string();
            out.emplace_back(std::max(0, c->depth - lead->depth), word + name);
        } else if (Starts(k, "GX|")) {
            out.emplace_back(0, "Switch Group " + c->label);
        } else if (Starts(k, "VX|")) {
            out.emplace_back(1, "Switch value " + c->label);
        } else if (Starts(k, "C|") && !has_value_line) {
            out.emplace_back(1, c->label);
        }
    }
    return out;
}

}  // namespace rav
