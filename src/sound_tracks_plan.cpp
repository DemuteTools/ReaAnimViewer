// SPDX-License-Identifier: MIT
//
// See sound_tracks_plan.h (story 13-13).

#include "sound_tracks_plan.h"

#include <algorithm>

#include "sound_record.h"

namespace rav {

namespace {

constexpr const char* kTrackKey = "trk=";
constexpr const char* kUnnamed = "(unnamed)";

bool IsTrackToken(const std::string& tok)
{
    return tok.compare(0, 4, kTrackKey) == 0;
}

const char* LayoutWord(TrackLayout l)
{
    return l == TrackLayout::Flat ? "flat" : l == TrackLayout::Tree ? "tree" : "";
}

std::string Named(const std::string& s)
{
    return s.empty() ? std::string(kUnnamed) : s;
}

std::string TrimmedLower(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    std::string out = s.substr(a, b - a);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}

}  // namespace

TrackLayout TrackLayoutOf(const SoundStore& store)
{
    for (const std::string& tok : store.kept) {
        if (!IsTrackToken(tok)) continue;
        const std::string v = tok.substr(4);
        if (v == "flat") return TrackLayout::Flat;
        if (v == "tree") return TrackLayout::Tree;
        return TrackLayout::Off;
    }
    return TrackLayout::Off;
}

bool SetTrackLayout(SoundStore& store, TrackLayout layout)
{
    if (TrackLayoutOf(store) == layout) return false;
    std::vector<std::string> kept;
    bool                     written = false;
    for (const std::string& tok : store.kept) {
        if (!IsTrackToken(tok)) {
            kept.push_back(tok);
            continue;
        }
        if (layout != TrackLayout::Off && !written) kept.push_back(std::string(kTrackKey) + LayoutWord(layout));
        written = true;
    }
    if (layout != TrackLayout::Off && !written) kept.push_back(std::string(kTrackKey) + LayoutWord(layout));
    store.kept = kept;
    return true;
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
            if (!hit) return {};  // on no value (Unassigned): never played through this Switch
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

std::vector<TrackStep> TrackPathFor(const std::vector<SwitchStep>& path, TrackLayout layout,
                                    const std::string& lands_in, const std::string& library_name)
{
    if (layout == TrackLayout::Off || path.empty()) return {};
    std::vector<TrackStep> out;
    out.push_back({Named(lands_in), true});
    out.push_back({Named(library_name), true});
    if (layout == TrackLayout::Flat) {
        std::string joined;
        for (size_t i = 0; i < path.size(); ++i) {
            if (i > 0) joined += '-';
            joined += Named(path[i].value_name);
        }
        out.push_back({joined, false});
    } else {
        for (size_t i = 0; i < path.size(); ++i) out.push_back({Named(path[i].value_name), i + 1 < path.size()});
    }
    return out;
}

int MatchChildTrack(const std::vector<std::string>& child_names, const std::string& name)
{
    const std::string want = TrimmedLower(name);
    for (size_t i = 0; i < child_names.size(); ++i)
        if (TrimmedLower(child_names[i]) == want) return static_cast<int>(i);
    return -1;
}

}  // namespace rav
