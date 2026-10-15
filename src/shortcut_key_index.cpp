// SPDX-License-Identifier: MIT
//
// See shortcut_key_index.h.

#include "shortcut_key_index.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <tuple>
#include <vector>

#include "action_link.h"

namespace rav {
namespace {

std::string Fold(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '\t') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

void AddGesture(std::vector<KbGesture>& gs, KbGesture g, KbGestureNormalizer norm)
{
    if (norm && !norm(&g)) return;
    if (std::find(gs.begin(), gs.end(), g) == gs.end()) gs.push_back(g);
}

}  // namespace

bool KbGestureLess(const KbGesture& a, const KbGesture& b)
{
    return std::make_tuple(static_cast<int>(a.input), a.mods, a.is_char, a.code) <
           std::make_tuple(static_cast<int>(b.input), b.mods, b.is_char, b.code);
}

std::vector<int> ShortcutKeyIndex::ActionsOn(const KbGesture& g) const
{
    std::vector<int> out;
    Entry probe;
    probe.g = g;
    probe.action = -1;
    auto it = std::lower_bound(by_gesture.begin(), by_gesture.end(), probe, [](const Entry& x, const Entry& y) {
        if (KbGestureLess(x.g, y.g)) return true;
        if (KbGestureLess(y.g, x.g)) return false;
        return x.action < y.action;
    });
    for (; it != by_gesture.end() && it->g == g; ++it) out.push_back(it->action);
    return out;
}

ShortcutKeyIndex BuildShortcutKeyIndex(const std::vector<KeyIndexSource>& src, const ViewKeymap* kb,
                                       KbGestureNormalizer norm)
{
    ShortcutKeyIndex ix;
    ix.actions.reserve(src.size());
    // The commands reaper-kb.ini's KEY lines name (folded), so only those ask KbGesturesOf.
    std::vector<std::string> in_file;
    if (kb) {
        for (const KbLine& l : kb->lines) {
            const std::string n = Fold(NormalizeCommandId(l.cmd));
            if (!n.empty()) in_file.push_back(n);
        }
        std::sort(in_file.begin(), in_file.end());
        in_file.erase(std::unique(in_file.begin(), in_file.end()), in_file.end());
    }
    for (const KeyIndexSource& s : src) {
        KeyIndexAction a;
        a.id = s.id;
        a.name = s.name;
        for (const std::string& d : s.shortcuts) {
            a.descs.push_back(d);
            KbGesture g;
            if (ParseReaperShortcutDesc(d, &g)) AddGesture(a.gestures, g, norm);
        }
        if (kb && !in_file.empty() && std::binary_search(in_file.begin(), in_file.end(), Fold(NormalizeCommandId(s.id))))
            for (const KbGesture& g : KbGesturesOf(*kb, s.id)) AddGesture(a.gestures, g, norm);
        const int i = static_cast<int>(ix.actions.size());
        for (const KbGesture& g : a.gestures) ix.by_gesture.push_back(ShortcutKeyIndex::Entry{g, i});
        ix.actions.push_back(std::move(a));
    }
    std::stable_sort(ix.by_gesture.begin(), ix.by_gesture.end(),
                     [](const ShortcutKeyIndex::Entry& x, const ShortcutKeyIndex::Entry& y) { return KbGestureLess(x.g, y.g); });
    return ix;
}

bool ParseTypedShortcut(const std::string& text, KbGesture* out, KbGestureNormalizer norm)
{
    const std::string t = Trim(text);
    if (t.empty()) return false;
    // One character alone is a word of a name ("s"), never a key: a shortcut is typed with a modifier or
    // by its key's name ("Space", "F5", "Wheel").
    size_t code_points = 0;
    for (char c : t)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++code_points;
    if (code_points == 1) return false;
    KbGesture g;
    if (!ParseReaperShortcutDesc(t, &g)) return false;
    if (norm && !norm(&g)) return false;
    if (out) *out = g;
    return true;
}

bool LinkSearchMatch(const KeyIndexAction& a, const std::string& query, const KbGesture* typed)
{
    if (ActionSearchMatch(a.name, a.id, query)) return true;
    if (typed && std::find(a.gestures.begin(), a.gestures.end(), *typed) != a.gestures.end()) return true;
    // A partial shortcut being typed ("ctrl+sh"); a complete one (`typed`) matches its gesture only.
    const std::string q = Fold(query);
    if (!typed && q.find('+') != std::string::npos)
        for (const std::string& d : a.descs) {
            const std::string fd = Fold(d);
            if (fd.compare(0, q.size(), q) == 0) return true;
        }
    return false;
}

bool KeyBindingOfKbGesture(const KbGesture& g, KeyBinding* out)
{
    if (g.input != KbInput::Key || g.is_char || g.code <= 0) return false;
    KeyBinding b;
    b.vk = static_cast<unsigned>(g.code);
    b.ctrl = (g.mods & kViewModCtrl) != 0;
    b.shift = (g.mods & kViewModShift) != 0;
    b.alt = (g.mods & kViewModAlt) != 0;
    if (out) *out = b;
    return true;
}

KbGesture KbGestureOfBinding(const KeyBinding& b)
{
    KbGesture g;
    g.input = KbInput::Key;
    g.code = static_cast<int>(b.vk);
    g.mods = (b.ctrl ? kViewModCtrl : 0) | (b.shift ? kViewModShift : 0) | (b.alt ? kViewModAlt : 0);
    return g;
}

std::vector<RavKeyUse> RavUsesOf(const ShortcutKeymap& keys, const ViewNavMap& nav, const KbGesture& g, int char_view_row)
{
    std::vector<RavKeyUse> out;
    auto add = [&out](RavKeyUse::Kind k, int i, bool shadowed) {
        for (const RavKeyUse& u : out)
            if (u.kind == k && u.index == i) return;
        RavKeyUse u;
        u.kind = k;
        u.index = i;
        u.shadowed = shadowed;
        out.push_back(u);
    };
    KeyBinding kb;
    const bool is_key = KeyBindingOfKbGesture(g, &kb);
    if (is_key) {
        for (int i = 0; i < kShortcutCount; ++i) {
            const std::vector<KeyBinding>& ks = keys.keys[static_cast<size_t>(i)];
            if (std::find(ks.begin(), ks.end(), kb) != ks.end()) add(RavKeyUse::Kind::KeyRow, i, false);
        }
        for (int i = 0; i < kShortcutCount; ++i) {
            const std::vector<KeyBinding>& ks = keys.shadowed[static_cast<size_t>(i)];
            if (std::find(ks.begin(), ks.end(), kb) != ks.end()) add(RavKeyUse::Kind::KeyRow, i, true);
        }
    }
    const int m = g.mods & 7;
    if (g.input == KbInput::Wheel && nav.wheel[m].row >= 0) add(RavKeyUse::Kind::ViewRow, nav.wheel[m].row, false);
    if (g.input == KbInput::HWheel && nav.hwheel[m].row >= 0) add(RavKeyUse::Kind::ViewRow, nav.hwheel[m].row, false);
    if (g.input == KbInput::Key) {
        for (const auto& kv : nav.keys) {
            const ViewGesture& v = kv.first;
            if (v.kind == GestureKind::Key && v.code == g.code && v.is_char == g.is_char && v.mods == g.mods)
                add(RavKeyUse::Kind::ViewRow, kv.second, false);
        }
        if (char_view_row >= 0 && char_view_row < kNavRowCount) add(RavKeyUse::Kind::ViewRow, char_view_row, false);
    }
    if (is_key)
        for (int i = 0; i < kEditReservedCount; ++i)
            if (kEditReservedKeys[i].vk == kb.vk && kEditReservedKeys[i].mods == g.mods)
                add(RavKeyUse::Kind::EditReserved, i, false);
    int mi = 0;
    for (const MouseGestureDef& md : kMouseGestureTable) {
        KbGesture mg;
        if (ParseReaperShortcutDesc(md.gesture, &mg) && mg.input != KbInput::Key && mg == g) add(RavKeyUse::Kind::Mouse, mi, false);
        ++mi;
    }
    return out;
}

}  // namespace rav
