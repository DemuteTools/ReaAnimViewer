// SPDX-License-Identifier: MIT
//
// See clip_list_plan.h.

#include "clip_list_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>

namespace rav {
namespace cliplist {
namespace {

char LowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Lower(const std::string& s)
{
    std::string out(s);
    for (char& c : out) c = LowerAscii(c);
    return out;
}

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

// Time order, then the arrange's order, then the order read.
bool EarlierRow(const std::vector<ClipEntry>& e, int a, int b)
{
    const ClipEntry& x = e[static_cast<size_t>(a)];
    const ClipEntry& y = e[static_cast<size_t>(b)];
    if (x.pos != y.pos) return x.pos < y.pos;
    if (x.track_num != y.track_num) return x.track_num < y.track_num;
    return a < b;
}

}  // namespace

void NumberCopies(std::vector<ClipEntry>& entries)
{
    std::map<std::string, std::vector<int>> by_clip;
    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i].copy_n = entries[i].copy_of = 0;
        by_clip[Lower(entries[i].path) + '\n' + entries[i].clip].push_back(static_cast<int>(i));
    }
    for (auto& kv : by_clip) {
        std::vector<int>& rows = kv.second;
        if (rows.size() < 2) continue;
        std::sort(rows.begin(), rows.end(), [&](int a, int b) { return EarlierRow(entries, a, b); });
        for (size_t k = 0; k < rows.size(); ++k) {
            ClipEntry& e = entries[static_cast<size_t>(rows[k])];
            e.copy_n = static_cast<int>(k) + 1;
            e.copy_of = static_cast<int>(rows.size());
        }
    }
}

bool MatchesQuery(const ClipEntry& e, const std::string& query)
{
    const std::string q = Lower(Trim(query));
    if (q.empty()) return true;
    return Lower(e.clip + " " + e.file + " " + e.track).find(q) != std::string::npos;
}

ListView BuildListView(const std::vector<ClipEntry>& entries, Grouping g, const std::string& query)
{
    ListView v;
    v.total = static_cast<int>(entries.size());
    std::vector<int> kept;
    for (size_t i = 0; i < entries.size(); ++i)
        if (MatchesQuery(entries[i], query)) kept.push_back(static_cast<int>(i));
    std::sort(kept.begin(), kept.end(), [&](int a, int b) { return EarlierRow(entries, a, b); });
    v.shown = static_cast<int>(kept.size());

    if (g == Grouping::None) {
        if (!kept.empty()) {
            GroupView one;
            one.rows = kept;
            v.groups.push_back(std::move(one));
        }
    } else if (g == Grouping::Track) {
        std::map<int, size_t> at;  // track number -> group
        std::vector<int>      nums;
        for (int r : kept) {
            const ClipEntry& e = entries[static_cast<size_t>(r)];
            if (at.find(e.track_num) == at.end()) {
                at[e.track_num] = v.groups.size();
                nums.push_back(e.track_num);
                GroupView gv;
                gv.label = e.track;
                gv.rgb = e.track_rgb;
                gv.has_color = e.has_color;
                v.groups.push_back(std::move(gv));
            }
            v.groups[at[e.track_num]].rows.push_back(r);
        }
        std::stable_sort(v.groups.begin(), v.groups.end(), [&](const GroupView& a, const GroupView& b) {
            return entries[static_cast<size_t>(a.rows.front())].track_num <
                   entries[static_cast<size_t>(b.rows.front())].track_num;
        });
    } else {
        std::map<std::string, size_t> at;  // file (no case) -> group
        for (int r : kept) {
            const ClipEntry&  e = entries[static_cast<size_t>(r)];
            const std::string key = Lower(e.file);
            if (at.find(key) == at.end()) {
                at[key] = v.groups.size();
                GroupView gv;
                gv.label = e.file;
                v.groups.push_back(std::move(gv));
            }
            v.groups[at[key]].rows.push_back(r);
        }
        std::stable_sort(v.groups.begin(), v.groups.end(), [](const GroupView& a, const GroupView& b) {
            return Lower(a.label) < Lower(b.label);
        });
    }
    for (const GroupView& gv : v.groups) v.order.insert(v.order.end(), gv.rows.begin(), gv.rows.end());
    return v;
}

std::string SubLine(const ClipEntry& e, Grouping g)
{
    return (g == Grouping::File ? e.track : e.file) + " \xC2\xB7 " + FormatLength(e.len);
}

std::string FormatPosition(double seconds)
{
    if (!(seconds > 0.0) || !std::isfinite(seconds)) seconds = 0.0;
    // Tenths, rounded once (59.96 s is 1:00.0, never 0:60.0).
    const long long tenths = static_cast<long long>(std::floor(seconds * 10.0 + 0.5));
    const long long t = tenths % 10, s = (tenths / 10) % 60, m = (tenths / 600) % 60, h = tenths / 36000;
    char buf[48];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%lld", h, m, s, t);
    else
        std::snprintf(buf, sizeof(buf), "%lld:%02lld.%lld", m, s, t);
    return buf;
}

std::string FormatLength(double seconds)
{
    if (!(seconds > 0.0) || !std::isfinite(seconds)) seconds = 0.0;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.1f s", seconds);
    return buf;
}

std::string CopyBadge(const ClipEntry& e)
{
    if (e.copy_of < 2 || e.copy_n < 1) return std::string();
    return "copy " + std::to_string(e.copy_n) + "/" + std::to_string(e.copy_of);
}

int StepRow(const std::vector<int>& order, int kb, int cur, int delta)
{
    if (order.empty()) return -1;
    auto index_of = [&](int id) {
        const auto it = std::find(order.begin(), order.end(), id);
        return it == order.end() ? -1 : static_cast<int>(it - order.begin());
    };
    int i = kb >= 0 ? index_of(kb) : -1;
    if (i < 0) i = cur >= 0 ? index_of(cur) : -1;
    if (i < 0) return order.front();
    const int n = static_cast<int>(order.size());
    i = std::max(0, std::min(n - 1, i + (delta > 0 ? 1 : delta < 0 ? -1 : 0)));
    return order[static_cast<size_t>(i)];
}

std::vector<ListLine> FlattenListView(const ListView& lv)
{
    std::vector<ListLine> lines;
    lines.reserve(lv.order.size() + lv.groups.size());
    for (size_t gi = 0; gi < lv.groups.size(); ++gi) {
        const GroupView& gv = lv.groups[gi];
        if (!gv.label.empty()) lines.push_back(ListLine{static_cast<int>(gi), -1});
        for (int r : gv.rows) lines.push_back(ListLine{static_cast<int>(gi), r});
    }
    return lines;
}

std::vector<float> LineTops(const std::vector<ListLine>& lines, float header_h, float row_h, float* total)
{
    std::vector<float> tops;
    tops.reserve(lines.size());
    float y = 0.0f;
    for (const ListLine& l : lines) {
        tops.push_back(y);
        y += l.row < 0 ? header_h : row_h;
    }
    if (total) *total = y;
    return tops;
}

void VisibleLines(const std::vector<float>& tops, float total, float y0, float y1, int* first, int* last)
{
    int f = 0, l = 0;
    if (!tops.empty() && y1 > y0 && y1 > 0.0f && y0 < total) {
        // The first line whose bottom is below y0: the last top at or above y0.
        f = static_cast<int>(std::upper_bound(tops.begin(), tops.end(), y0) - tops.begin()) - 1;
        f = std::max(0, f);
        // The first line whose top is at or below y1 is past the end.
        l = static_cast<int>(std::lower_bound(tops.begin(), tops.end(), y1) - tops.begin());
        l = std::max(f, l);
    }
    if (first) *first = f;
    if (last) *last = l;
}

float PanelWidth(float view_w, float dp)
{
    if (!(view_w > 0.0f)) return 0.0f;
    // Every term is continuous in view_w, so the panel never jumps while the window is resized.
    const float full = std::max(0.0f, view_w - 16.0f * dp);  // the view less the close button's room
    return std::min({270.0f * dp, full, std::max(0.7f * view_w, std::min(full, 200.0f * dp))});
}

float ButtonCentreY(float menu_bottom, float switch_top, float btn, float dp)
{
    if (switch_top - menu_bottom < 140.0f * dp) return menu_bottom + 6.0f * dp + btn * 0.5f;
    return (menu_bottom + switch_top) * 0.5f;
}

bool ButtonFits(float menu_bottom, float switch_top, float btn, float dp)
{
    return switch_top - menu_bottom >= 6.0f * dp + btn + 4.0f * dp;
}

}  // namespace cliplist
}  // namespace rav
