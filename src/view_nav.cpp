// SPDX-License-Identifier: MIT
//
// See view_nav.h -- the pure half (host-tested by tests/view_nav_test.cpp).

#include "view_nav.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "action_link.h"  // 13-60
#include "shortcuts.h"  // ShortcutKeyName, vk::

namespace rav {

int ViewNavRowOf(ViewWheelAction a)
{
    switch (a) {
    case ViewWheelAction::ScrollH: return kNavScrollH;
    case ViewWheelAction::ScrollV: return kNavScrollV;
    case ViewWheelAction::ZoomH: return kNavZoomH;
    case ViewWheelAction::ZoomV: return kNavZoomV;
    default: return -1;
    }
}

ViewGesture WheelGesture(bool horizontal, int mods)
{
    ViewGesture g;
    g.kind = horizontal ? GestureKind::HWheel : GestureKind::Wheel;
    g.mods = mods & 7;
    return g;
}

ViewGesture KeyGesture(int vk, int mods)
{
    ViewGesture g;
    g.kind = GestureKind::Key;
    g.code = vk;
    g.mods = mods & 7;
    return g;
}

ViewGesture ViewGestureOf(const KbGesture& k)
{
    ViewGesture g;
    g.mods = k.mods & 7;
    if (k.input == KbInput::Key) {
        g.kind = GestureKind::Key;
        g.code = k.code;
        g.is_char = k.is_char;
    } else {
        g.kind = k.input == KbInput::Wheel ? GestureKind::Wheel : GestureKind::HWheel;
    }
    return g;
}

namespace {

ViewGesture KeyOf(const ViewKeyBinding& b)
{
    ViewGesture g;
    g.kind = GestureKind::Key;
    g.code = b.code;
    g.is_char = b.is_char;
    g.mods = b.mods & 7;
    return g;
}

std::string ModsText(int mods)
{
    std::string s;
    if (mods & kViewModCtrl) s += "Ctrl+";
    if (mods & kViewModShift) s += "Shift+";
    if (mods & kViewModAlt) s += "Alt+";
    return s;
}

void DropGesture(ViewNavMap& m, const ViewGesture& g)
{
    if (g.kind == GestureKind::Wheel) m.wheel[g.mods & 7] = ViewNavWheel{};
    else if (g.kind == GestureKind::HWheel) m.hwheel[g.mods & 7] = ViewNavWheel{};
    else if (g.kind == GestureKind::Key)
        m.keys.erase(std::remove_if(m.keys.begin(), m.keys.end(), [&g](const std::pair<ViewGesture, int>& k) { return k.first == g; }),
                     m.keys.end());
}

}  // namespace

std::string FormatViewGesture(const ViewGesture& g, std::string (*fallback)(unsigned))
{
    switch (g.kind) {
    case GestureKind::Wheel: return ModsText(g.mods) + "Wheel";
    case GestureKind::HWheel: return ModsText(g.mods) + "H-wheel";
    case GestureKind::Key: {
        if (g.is_char) {
            if (g.code > 32 && g.code < 127) return ModsText(g.mods) + std::string(1, static_cast<char>(g.code));
            char buf[24];
            std::snprintf(buf, sizeof(buf), "Char %d", g.code);
            return ModsText(g.mods) + buf;
        }
        return ModsText(g.mods) + ShortcutKeyName(static_cast<unsigned>(g.code), fallback);
    }
    default: return std::string();
    }
}

ViewNavMap ResolveViewNav(const ViewKeymap& reaper, const ViewNavChoices& c)
{
    ViewNavMap m;
    auto linked = [&c](int row) { return row >= 0 && row < kNavRowCount && c[static_cast<size_t>(row)].linked_default(); };
    for (int i = 0; i < 8; ++i) {
        const ViewWheelBinding& w = reaper.wheel[i];
        const ViewWheelBinding& h = reaper.hwheel[i];
        const int rw = ViewNavRowOf(w.action), rh = ViewNavRowOf(h.action);
        if (linked(rw)) m.wheel[i] = ViewNavWheel{rw, w.reversed, w.page};
        if (linked(rh)) m.hwheel[i] = ViewNavWheel{rh, h.reversed, h.page};
    }
    if (linked(kNavZoomIn))
        for (const ViewKeyBinding& b : reaper.zoom_in) m.keys.emplace_back(KeyOf(b), kNavZoomIn);
    if (linked(kNavZoomOut))
        for (const ViewKeyBinding& b : reaper.zoom_out) m.keys.emplace_back(KeyOf(b), kNavZoomOut);
    // 13-60: rows linked to an action the user picked, over the default-linked ones.
    for (int row = 0; row < kNavRowCount; ++row) {
        const ViewNavChoice& ch = c[static_cast<size_t>(row)];
        if (ch.own || ch.cmd.empty()) continue;
        // A REAPER view action keeps its reversed / one-page (the user may pick 977, 976...).
        const std::string id = NormalizeCommandId(ch.cmd);
        const ViewWheelBinding vb =
            (!id.empty() && id[0] != '_') ? ViewWheelBindingOf(std::atoi(id.c_str())) : ViewWheelBinding{};
        for (const KbGesture& kg : KbGesturesOf(reaper, ch.cmd)) {
            const ViewGesture g = ViewGestureOf(kg);
            if (g.kind != GestureKind::Key && !kViewNavTable[row].wheel_ok) continue;
            DropGesture(m, g);
            const ViewNavWheel w{row, vb.action != ViewWheelAction::None && vb.reversed,
                                 vb.action != ViewWheelAction::None && vb.page};
            if (g.kind == GestureKind::Wheel) m.wheel[g.mods] = w;
            else if (g.kind == GestureKind::HWheel) m.hwheel[g.mods] = w;
            else m.keys.emplace_back(g, row);
        }
    }
    for (int row = 0; row < kNavRowCount; ++row) {
        const ViewNavChoice& ch = c[static_cast<size_t>(row)];
        if (!ch.own || ch.g.kind == GestureKind::None) continue;
        if (ch.g.kind != GestureKind::Key && !kViewNavTable[row].wheel_ok) continue;
        DropGesture(m, ch.g);  // the user's own gesture wins over REAPER's
        if (ch.g.kind == GestureKind::Wheel) m.wheel[ch.g.mods & 7] = ViewNavWheel{row, false, false};
        else if (ch.g.kind == GestureKind::HWheel) m.hwheel[ch.g.mods & 7] = ViewNavWheel{row, false, false};
        else m.keys.emplace_back(ch.g, row);
    }
    return m;
}

std::vector<ViewGesture> ViewNavGestures(const ViewNavMap& m, int row)
{
    std::vector<ViewGesture> out;
    if (row < 0) return out;
    for (int i = 0; i < 8; ++i)
        if (m.wheel[i].row == row) out.push_back(WheelGesture(false, i));
    for (int i = 0; i < 8; ++i)
        if (m.hwheel[i].row == row) out.push_back(WheelGesture(true, i));
    for (const auto& k : m.keys)
        if (k.second == row && std::find(out.begin(), out.end(), k.first) == out.end()) out.push_back(k.first);
    return out;
}

std::vector<ViewGesture> ReaperViewGestures(const ViewKeymap& reaper, int row)
{
    return ViewNavGestures(ResolveViewNav(reaper, ViewNavChoices{}), row);
}

int ViewNavOwner(const ViewNavMap& m, const ViewGesture& g)
{
    switch (g.kind) {
    case GestureKind::Wheel: return m.wheel[g.mods & 7].row;
    case GestureKind::HWheel: return m.hwheel[g.mods & 7].row;
    case GestureKind::Key:
        for (const auto& k : m.keys)
            if (k.first == g) return k.second;
        return -1;
    default: return -1;
    }
}

int ViewNavKeyRow(const ViewNavMap& m, int vk, int mods)
{
    for (const auto& k : m.keys)
        if (!k.first.is_char && k.first.code == vk && k.first.mods == (mods & 7)) return k.second;
    return -1;
}

int ViewNavCharRow(const ViewNavMap& m, int ch, int mods)
{
    if (ch <= 0) return -1;
    const int need = mods & (kViewModCtrl | kViewModAlt);
    for (const auto& k : m.keys)
        if (k.first.is_char && k.first.code == ch && (k.first.mods & (kViewModCtrl | kViewModAlt)) == need) return k.second;
    return -1;
}

namespace {

void AddWheel(ViewNavStep& s, const ViewNavWheel& w, double v)
{
    if (w.row < 0 || v == 0.0) return;
    if (w.reversed) v = -v;
    // v > 0: the wheel up (or the horizontal wheel left): back / in.
    switch (w.row) {
    case kNavZoomH: s.zoom_h += v; break;
    case kNavZoomV: s.zoom_v += v; break;
    case kNavScrollH: (w.page ? s.scroll_h_pages : s.scroll_h) -= v; break;
    case kNavScrollV: (w.page ? s.scroll_v_pages : s.scroll_v) -= v; break;
    default: break;
    }
}

}  // namespace

ViewNavStep ViewNavStepFor(const ViewNavMap& m, int mods, float wheel, float hwheel, const std::vector<int>& key_rows)
{
    ViewNavStep s;
    AddWheel(s, m.wheel[mods & 7], static_cast<double>(wheel));
    AddWheel(s, m.hwheel[mods & 7], static_cast<double>(hwheel));
    for (int row : key_rows) {
        switch (row) {
        case kNavZoomIn:
        case kNavZoomH: s.zoom_h += 2.0; break;
        case kNavZoomOut: s.zoom_h -= 2.0; break;
        case kNavZoomV: s.zoom_v += 2.0; break;
        case kNavScrollH: s.scroll_h += 1.0; break;
        case kNavScrollV: s.scroll_v += 1.0; break;
        default: break;
        }
    }
    return s;
}

bool ViewNavWheelBound(const ViewNavMap& m, int mods, bool horizontal)
{
    return (horizontal ? m.hwheel : m.wheel)[mods & 7].row >= 0;
}

bool ViewNavWheelYields(int mods)
{
    return (mods & (kViewModCtrl | kViewModAlt)) == 0;
}

bool KbShouldRead(KbWatch& w, const KbStamp& st, double now, double settle)
{
    if (!w.applied_once) return true;
    if (st == w.applied) {
        w.waiting = false;  // back as it was: nothing to read
        return false;
    }
    if (!w.waiting || st != w.seen) {  // a new stamp: wait until it stops changing
        w.waiting = true;
        w.seen = st;
        w.seen_at = now;
        return false;
    }
    return now - w.seen_at >= settle;
}

void KbApplied(KbWatch& w, const KbStamp& stamp)
{
    w.applied_once = true;
    w.applied = stamp;
    w.waiting = false;
}

NavRecordOutcome ViewNavRecordStep(const ViewNavMap& m, int row, const ViewGesture& g, int* owner)
{
    if (owner) *owner = -1;
    if (row < 0 || row >= kNavRowCount || g.kind == GestureKind::None) return NavRecordOutcome::Ignored;
    if (g.kind != GestureKind::Key && !kViewNavTable[row].wheel_ok) return NavRecordOutcome::Ignored;
    if (g.kind == GestureKind::Key && (g.is_char ? g.code <= 0 : !IsBindableKey(static_cast<unsigned>(g.code))))
        return NavRecordOutcome::Ignored;
    const int o = ViewNavOwner(m, g);
    if (o >= 0 && o != row) {
        if (owner) *owner = o;
        return NavRecordOutcome::Conflict;
    }
    return NavRecordOutcome::Assigned;
}

void ViewNavAssign(ViewNavChoices& c, int row, const ViewGesture& g)
{
    if (row < 0 || row >= kNavRowCount) return;
    for (int i = 0; i < kNavRowCount; ++i)
        if (i != row && c[static_cast<size_t>(i)].own && c[static_cast<size_t>(i)].g == g) c[static_cast<size_t>(i)].g = ViewGesture{};
    c[static_cast<size_t>(row)].own = true;
    c[static_cast<size_t>(row)].g = g;
    c[static_cast<size_t>(row)].cmd.clear();  // 13-60: no longer linked to an action
}

std::string EncodeViewNavChoice(const ViewNavChoice& c)
{
    if (!c.own) return c.cmd.empty() ? std::string() : EncodeActionLink(c.cmd);
    if (c.g.kind == GestureKind::None) return "own";
    std::string s = "own:" + ModsText(c.g.mods);
    if (c.g.kind == GestureKind::Wheel) s += "Wheel";
    else if (c.g.kind == GestureKind::HWheel) s += "HWheel";
    else if (c.g.is_char) s += "char" + std::to_string(c.g.code);
    else s += std::to_string(c.g.code);
    return s;
}

bool DecodeViewNavChoice(const char* text, ViewNavChoice* out)
{
    if (!text || !out) return false;
    if (!text[0]) {
        *out = ViewNavChoice{};
        return true;
    }
    std::string cmd;
    if (DecodeActionLink(text, &cmd)) {  // 13-60
        *out = ViewNavChoice{};
        out->cmd = cmd;
        return true;
    }
    if (std::strcmp(text, "own") == 0) {
        *out = ViewNavChoice{};
        out->own = true;
        return true;
    }
    if (std::strncmp(text, "own:", 4) != 0) return false;
    const char* p = text + 4;
    ViewGesture g;
    for (;;) {
        if (std::strncmp(p, "Ctrl+", 5) == 0 && !(g.mods & kViewModCtrl)) { g.mods |= kViewModCtrl; p += 5; }
        else if (std::strncmp(p, "Shift+", 6) == 0 && !(g.mods & kViewModShift)) { g.mods |= kViewModShift; p += 6; }
        else if (std::strncmp(p, "Alt+", 4) == 0 && !(g.mods & kViewModAlt)) { g.mods |= kViewModAlt; p += 4; }
        else break;
    }
    auto number = [](const char* q, long max, int* v) {
        if (*q < '0' || *q > '9') return false;
        char* end = nullptr;
        const long n = std::strtol(q, &end, 10);
        if (!end || *end != '\0' || n <= 0 || n > max) return false;
        *v = static_cast<int>(n);
        return true;
    };
    if (std::strcmp(p, "Wheel") == 0) g.kind = GestureKind::Wheel;
    else if (std::strcmp(p, "HWheel") == 0) g.kind = GestureKind::HWheel;
    else if (std::strncmp(p, "char", 4) == 0) {
        g.kind = GestureKind::Key;
        g.is_char = true;
        if (!number(p + 4, 0x10FFFF, &g.code)) return false;
    } else {
        g.kind = GestureKind::Key;
        if (!number(p, 0xFE, &g.code) || !IsBindableKey(static_cast<unsigned>(g.code))) return false;
    }
    out->own = true;
    out->g = g;
    return true;
}

std::string ViewNavHint(const ViewNavMap& m, std::string (*fallback)(unsigned))
{
    std::string s;
    const std::vector<ViewGesture> z = ViewNavGestures(m, kNavZoomH), sc = ViewNavGestures(m, kNavScrollH);
    if (!z.empty()) s += "Zoom: " + FormatViewGesture(z.front(), fallback);
    if (!sc.empty()) s += (s.empty() ? "" : "  \xC2\xB7  ") + std::string("Scroll: ") + FormatViewGesture(sc.front(), fallback);
    return s;
}

}  // namespace rav
