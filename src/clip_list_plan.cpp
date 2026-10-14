// SPDX-License-Identifier: MIT
//
// See clip_list_plan.h.

#include "clip_list_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
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
                gv.key = GroupKey(e, g);
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
                gv.key = GroupKey(e, g);
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

std::string GroupKey(const ClipEntry& e, Grouping g)
{
    if (g == Grouping::Track) return "t:" + (e.track_key.empty() ? "#" + std::to_string(e.track_num) : e.track_key);
    if (g == Grouping::File) return "f:" + Lower(e.file);
    return std::string();
}

void ApplyFolds(ListView& lv, const FoldSet& folded, bool searching)
{
    lv.order.clear();
    for (GroupView& gv : lv.groups) {
        gv.folded = !searching && !gv.key.empty() && !gv.label.empty() && folded.count(gv.key) != 0;
        if (!gv.folded) lv.order.insert(lv.order.end(), gv.rows.begin(), gv.rows.end());
    }
}

bool CanExpandAll(const ListView& lv, const FoldSet& folded, bool searching)
{
    if (searching) return false;
    for (const GroupView& gv : lv.groups)
        if (!gv.key.empty() && folded.count(gv.key) != 0) return true;
    return false;
}

bool CanCollapseAll(const ListView& lv, const FoldSet& folded, bool searching)
{
    if (searching) return false;
    for (const GroupView& gv : lv.groups)
        if (!gv.key.empty() && !gv.label.empty() && folded.count(gv.key) == 0) return true;
    return false;
}

void SetAllFolds(const ListView& lv, bool fold, FoldSet* folded)
{
    if (!folded) return;
    for (const GroupView& gv : lv.groups) {
        if (gv.key.empty() || gv.label.empty()) continue;
        if (fold) folded->insert(gv.key);
        else folded->erase(gv.key);
    }
}

void ToggleFold(const std::string& key, FoldSet* folded)
{
    if (!folded || key.empty()) return;
    if (!folded->erase(key)) folded->insert(key);
}

int SelectionFocus(const std::vector<int>& before, const std::vector<int>& now)
{
    for (int r : now)
        if (r >= 0 && std::find(before.begin(), before.end(), r) == before.end()) return r;
    return -1;
}

PanelSpan PanelVerticalSpan(float menu_bottom, float ui_top, float view_bottom, float dp)
{
    // 6-5-10c: an edge not known (not a number) is the next one known: never a 1 px panel.
    if (!std::isfinite(menu_bottom)) menu_bottom = 0.0f;
    if (!std::isfinite(view_bottom)) view_bottom = std::isfinite(ui_top) ? ui_top : menu_bottom + 168.0f * dp;
    if (!std::isfinite(ui_top)) ui_top = view_bottom;
    PanelSpan s;
    const float floor_y = std::max(1.0f, view_bottom);  // never over the strip / the timeline
    const float min_h = 160.0f * dp;
    s.top = std::max(0.0f, menu_bottom + 8.0f * dp);
    s.bottom = std::min(floor_y, ui_top - 8.0f * dp);
    if (s.bottom - s.top < min_h) {
        // Too short: over the bottom-left controls first (down to the strip), then over the menu.
        s.bottom = std::min(floor_y, std::max(s.bottom, s.top + min_h));
        if (s.bottom - s.top < min_h) s.top = std::max(0.0f, s.bottom - min_h);
    }
    if (s.top > s.bottom - 1.0f) s.top = std::max(0.0f, s.bottom - 1.0f);
    return s;
}

std::vector<ListLine> FlattenListView(const ListView& lv)
{
    std::vector<ListLine> lines;
    lines.reserve(lv.order.size() + lv.groups.size());
    for (size_t gi = 0; gi < lv.groups.size(); ++gi) {
        const GroupView& gv = lv.groups[gi];
        if (!gv.label.empty()) lines.push_back(ListLine{static_cast<int>(gi), -1});
        if (gv.folded) continue;
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
    const float full = std::max(0.0f, view_w - 28.0f * dp);  // the view less the close button's room
    return std::min({270.0f * dp, full, std::max(0.7f * view_w, std::min(full, 200.0f * dp))});
}

float ButtonCentreY(float menu_bottom, float switch_top, float btn, float dp)
{
    if (switch_top - menu_bottom < 140.0f * dp) return menu_bottom + 6.0f * dp + btn * 0.5f;
    return (menu_bottom + switch_top) * 0.5f;
}

float OpenButtonCentreX(float panel_w, float btn, float dp)
{
    return panel_w + 2.0f * dp + btn * 0.5f;
}

bool ListTypesKey(unsigned vk, bool ctrl, bool alt)
{
    if (ctrl || alt) return false;
    if (vk >= 0x30 && vk <= 0x39) return true;  // 0-9
    if (vk >= 0x41 && vk <= 0x5A) return true;  // A-Z
    // The punctuation keys (VK_OEM_1..VK_OEM_3, VK_OEM_4..VK_OEM_8, VK_OEM_102): ; = , - . / ` [ \ ] ' < >
    if (vk >= 0xBA && vk <= 0xC0) return true;
    if (vk >= 0xDB && vk <= 0xDF) return true;
    return vk == 0xE2;
}

bool ButtonFits(float menu_bottom, float switch_top, float btn, float dp)
{
    return switch_top - menu_bottom >= 6.0f * dp + btn + 4.0f * dp;
}

MenuButtonBox MenuButtonAt(float row_mid, float dp)
{
    MenuButtonBox b;
    b.size = std::round(24.0f * dp);
    b.x = std::round(18.0f * dp);  // whole pixels: crisp at 125 %
    // Centred on the row (whole pixels), never closer than 10 px to the window's top.
    b.y = std::max(10.0f * dp, std::round(row_mid - b.size * 0.5f));
    b.bottom = b.y + b.size;
    b.panel_x = b.x - 8.0f * dp;
    b.panel_y = b.y - 8.0f * dp;
    b.trail = b.x + b.size + 8.0f * dp;
    return b;
}

LoadVerdict ProjectLoadVerdict(const ProjectLook& before, const ProjectLook& now, int listed, int listed_still)
{
    if (now.proj != before.proj) return LoadVerdict::OtherTab;
    if (now.path != before.path) return LoadVerdict::OtherFile;
    if (before.items > 0 && now.items == 0) return LoadVerdict::Emptied;
    // A real load makes new items: the list's old ones are gone (a few may come back at the same
    // address; most do not).
    if (listed > 0 && 2 * std::max(0, listed_still) < listed) return LoadVerdict::Replaced;
    return LoadVerdict::Stays;
}

const char* LoadVerdictText(LoadVerdict v)
{
    switch (v) {
    case LoadVerdict::OtherTab: return "another project tab";
    case LoadVerdict::OtherFile: return "another file";
    case LoadVerdict::Emptied: return "an empty project";
    case LoadVerdict::Replaced: return "the same file opened again";
    default: return "nothing opened";
    }
}

std::string CloseNote(const std::string& view, long long frames_since_click, double seconds_since_click)
{
    std::string s = view.empty() ? std::string("viewer") : view + " view";
    if (frames_since_click >= 0 && seconds_since_click >= 0.0 && seconds_since_click < 10.0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), ", %lld frame%s (%.2f s) after a row click", frames_since_click,
                      frames_since_click == 1 ? "" : "s", seconds_since_click);
        s += buf;
    }
    return s;
}

}  // namespace cliplist
}  // namespace rav
