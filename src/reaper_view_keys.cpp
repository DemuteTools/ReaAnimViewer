// SPDX-License-Identifier: MIT
//
// See reaper_view_keys.h.

#include "reaper_view_keys.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sstream>

#include "action_link.h"

namespace rav {

namespace {

constexpr int kVkAdd = 0x6B, kVkSubtract = 0x6D, kVkOemPlus = 0xBB, kVkOemMinus = 0xBD, kVkUp = 0x26,
              kVkDown = 0x28;

ViewKeyBinding Vk(int code, int mods = 0)
{
    ViewKeyBinding b;
    b.code = code;
    b.mods = mods;
    return b;
}

void Remove(std::vector<ViewKeyBinding>& v, const ViewKeyBinding& b)
{
    v.erase(std::remove(v.begin(), v.end(), b), v.end());
}

void Add(std::vector<ViewKeyBinding>& v, const ViewKeyBinding& b)
{
    if (std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
}

}  // namespace

ViewWheelBinding ViewWheelBindingOf(int command)
{
    ViewWheelBinding b;
    auto set = [&b](ViewWheelAction a, bool reversed, bool page) {
        b.action = a;
        b.reversed = reversed;
        b.page = page;
    };
    switch (command) {
    case 988: set(ViewWheelAction::ScrollH, false, false); break;
    case 977: set(ViewWheelAction::ScrollH, true, false); break;
    case 981: set(ViewWheelAction::ScrollH, false, true); break;
    case 975: set(ViewWheelAction::ScrollH, true, true); break;
    case 989: set(ViewWheelAction::ScrollV, false, false); break;
    case 978: set(ViewWheelAction::ScrollV, true, false); break;
    case 976: set(ViewWheelAction::ScrollV, false, true); break;
    case 982: set(ViewWheelAction::ScrollV, true, true); break;
    case 990: set(ViewWheelAction::ZoomH, false, false); break;
    case 979: set(ViewWheelAction::ZoomH, true, false); break;
    case 1000:
    case 991: set(ViewWheelAction::ZoomV, false, false); break;
    case 1001:
    case 980: set(ViewWheelAction::ZoomV, true, false); break;
    default: break;
    }
    return b;
}

ViewWheelAction ViewWheelActionOf(int command)
{
    return ViewWheelBindingOf(command).action;
}

ViewKeymap DefaultViewKeymap()
{
    ViewKeymap k;
    // REAPER 7.81's default table (reaper_view_keys.h); 253 -> 972 (track heights) is not ours.
    k.wheel[0] = ViewWheelBindingOf(990);
    k.wheel[kViewModCtrl] = ViewWheelBindingOf(1000);
    k.wheel[kViewModAlt] = ViewWheelBindingOf(988);
    k.wheel[kViewModCtrl | kViewModAlt] = ViewWheelBindingOf(989);
    k.wheel[kViewModAlt | kViewModShift] = ViewWheelBindingOf(976);
    k.hwheel[0] = ViewWheelBindingOf(988);
    k.hwheel[kViewModAlt] = ViewWheelBindingOf(990);
    k.zoom_in = {Vk(kVkAdd), Vk(kVkOemPlus), Vk(kVkOemPlus, kViewModShift), Vk(kVkUp)};
    k.zoom_out = {Vk(kVkSubtract), Vk(kVkOemMinus), Vk(kVkDown)};
    return k;
}

ViewKeymap ParseViewKeymap(const std::string& text)
{
    ViewKeymap         k = DefaultViewKeymap();
    std::istringstream in(text);
    std::string        line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string        tag, cmd_s;
        long               mod = 0, key = 0, section = -1;
        if (!(ls >> tag) || tag != "KEY") continue;
        if (!(ls >> mod >> key >> cmd_s >> section) || section != 0) continue;
        // 13-60: every line is kept, any command (a later one for the same input replaces it).
        {
            KbLine kl;
            kl.mod = static_cast<int>(mod);
            kl.key = static_cast<int>(key);
            kl.cmd = cmd_s;
            k.lines.erase(std::remove_if(k.lines.begin(), k.lines.end(),
                                         [&kl](const KbLine& o) { return o.mod == kl.mod && o.key == kl.key; }),
                          k.lines.end());
            k.lines.push_back(kl);
        }
        // Custom actions are "_NAME" strings: none of ours, but they do take the key.
        int cmd = -1;
        if (!cmd_s.empty() && cmd_s[0] != '_') {
            try {
                cmd = std::stoi(cmd_s);
            } catch (...) {
                continue;
            }
        }
        if (mod == 255) {
            // The input in the high nibble, Ctrl / Alt / Shift in bits 0-2, bit 3 clear = Win held.
            if (key < 0 || key > 255 || (key & 8) == 0) continue;
            const ViewWheelBinding b = ViewWheelBindingOf(cmd);
            if ((key & 0xF0) == 0xF0) k.wheel[key & 7] = b;
            else if ((key & 0xF0) == 0xD0) k.hwheel[key & 7] = b;
            continue;
        }
        if (mod < 0 || mod > 127 || key <= 0) continue;
        if (mod & 32) continue;  // the Win key: no modifier of ours stands for it, so never matched
        ViewKeyBinding b;
        b.is_char = (mod & 1) == 0;
        b.code = static_cast<int>(key);
        b.mods = ((mod & 8) ? kViewModCtrl : 0) | ((mod & 16) ? kViewModAlt : 0) | ((mod & 4) ? kViewModShift : 0);
        // A key bound in the file runs that command only (a default it had is taken over).
        Remove(k.zoom_in, b);
        Remove(k.zoom_out, b);
        if (cmd == 1012) Add(k.zoom_in, b);
        else if (cmd == 1011) Add(k.zoom_out, b);
    }
    return k;
}

std::vector<KbLine> DefaultKbLines()
{
    // DefaultViewKeymap's table as KEY lines: the wheel codes (255 + REAPER's code) and the zoom keys
    // (<mod> 1 = a virtual key, + 4 Shift).
    return {
        {255, 248, "990"}, {255, 249, "1000"}, {255, 250, "988"}, {255, 251, "989"}, {255, 254, "976"},
        {255, 216, "988"}, {255, 218, "990"},
        {1, kVkAdd, "1012"}, {1, kVkOemPlus, "1012"}, {5, kVkOemPlus, "1012"}, {1, kVkUp, "1012"},
        {1, kVkSubtract, "1011"}, {1, kVkOemMinus, "1011"}, {1, kVkDown, "1011"},
    };
}

namespace {

// The lines in force: the defaults the file has not taken over, then the file's.
std::vector<KbLine> LinesInForce(const ViewKeymap& k)
{
    std::vector<KbLine> all;
    for (const KbLine& d : DefaultKbLines()) {
        const bool taken = std::any_of(k.lines.begin(), k.lines.end(),
                                       [&d](const KbLine& l) { return l.mod == d.mod && l.key == d.key; });
        if (!taken) all.push_back(d);
    }
    all.insert(all.end(), k.lines.begin(), k.lines.end());
    return all;
}

}  // namespace

std::vector<KbGesture> KbGesturesOf(const ViewKeymap& k, const std::string& command)
{
    std::vector<KbGesture> out;
    if (NormalizeCommandId(command).empty()) return out;
    for (const KbLine& l : LinesInForce(k)) {
        if (!SameCommandId(l.cmd, command)) continue;
        KbGesture g;
        if (l.mod == 255) {
            // As ParseViewKeymap: the input in the high nibble, bit 3 clear = Win held.
            if (l.key < 0 || l.key > 255 || (l.key & 8) == 0) continue;
            if ((l.key & 0xF0) == 0xF0) g.input = KbInput::Wheel;
            else if ((l.key & 0xF0) == 0xD0) g.input = KbInput::HWheel;
            else continue;
            g.mods = l.key & 7;
        } else {
            if (l.mod < 0 || l.mod > 127 || l.key <= 0 || (l.mod & 32)) continue;
            g.input = KbInput::Key;
            g.is_char = (l.mod & 1) == 0;
            g.code = l.key;
            g.mods = ((l.mod & 8) ? kViewModCtrl : 0) | ((l.mod & 16) ? kViewModAlt : 0) | ((l.mod & 4) ? kViewModShift : 0);
        }
        if (std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
    }
    for (const auto& l : k.listed)
        if (SameCommandId(l.first, command) && std::find(out.begin(), out.end(), l.second) == out.end())
            out.push_back(l.second);
    return out;
}

bool ParseReaperShortcutDesc(const std::string& desc, KbGesture* out)
{
    std::string s = desc;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t p = 0;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    auto starts = [&s, &p](const char* w) {
        const size_t n = std::strlen(w);
        if (s.size() - p < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(s[p + i])) != std::tolower(static_cast<unsigned char>(w[i]))) return false;
        return true;
    };
    KbGesture g;
    g.input = KbInput::Key;
    // The modifiers, each once, then the key (which may itself be "+": "Ctrl++").
    for (;;) {
        if (s.size() - p > 5 && starts("ctrl+") && !(g.mods & kViewModCtrl)) { g.mods |= kViewModCtrl; p += 5; }
        else if (s.size() - p > 4 && starts("alt+") && !(g.mods & kViewModAlt)) { g.mods |= kViewModAlt; p += 4; }
        else if (s.size() - p > 6 && starts("shift+") && !(g.mods & kViewModShift)) { g.mods |= kViewModShift; p += 6; }
        else break;
    }
    const std::string key = s.substr(p);
    if (key.empty()) return false;
    std::string lk = key;
    for (char& c : lk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (key.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(key[0]);
        if (c <= 32 || c >= 127) return false;
        if (std::isalpha(c)) g.code = std::toupper(c);  // a letter's virtual key
        else if (std::isdigit(c)) g.code = c;
        else {
            g.is_char = true;  // "+", "=", "[": the key typing it (the caller's layout)
            g.code = c;
        }
        if (out) *out = g;
        return true;
    }
    if ((lk[0] == 'f') && lk.size() <= 3 && std::isdigit(static_cast<unsigned char>(lk[1])) &&
        (lk.size() == 2 || std::isdigit(static_cast<unsigned char>(lk[2])))) {
        const int n = std::atoi(lk.c_str() + 1);
        if (n < 1 || n > 24) return false;
        g.code = 0x70 + n - 1;
        if (out) *out = g;
        return true;
    }
    struct Named {
        const char* name;
        int         vk;
    };
    static const Named kNamed[] = {
        {"space", 0x20}, {"enter", 0x0D}, {"return", 0x0D}, {"tab", 0x09}, {"backspace", 0x08},
        {"delete", 0x2E}, {"del", 0x2E}, {"insert", 0x2D}, {"ins", 0x2D}, {"home", 0x24}, {"end", 0x23},
        {"pageup", 0x21}, {"page up", 0x21}, {"pgup", 0x21}, {"pagedown", 0x22}, {"page down", 0x22},
        {"pgdn", 0x22}, {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28}, {"pause", 0x13},
        {"num *", 0x6A}, {"num +", 0x6B}, {"num -", 0x6D}, {"num .", 0x6E}, {"num /", 0x6F},
        {"numpad *", 0x6A}, {"numpad +", 0x6B}, {"numpad -", 0x6D}, {"numpad .", 0x6E}, {"numpad /", 0x6F},
    };
    for (const Named& n : kNamed)
        if (lk == n.name) {
            g.code = n.vk;
            if (out) *out = g;
            return true;
        }
    for (const char* pre : {"num ", "numpad "}) {
        const size_t n = std::strlen(pre);
        if (lk.size() == n + 1 && lk.compare(0, n, pre) == 0 && std::isdigit(static_cast<unsigned char>(lk[n]))) {
            g.code = 0x60 + (lk[n] - '0');
            if (out) *out = g;
            return true;
        }
    }
    return false;
}

}  // namespace rav
