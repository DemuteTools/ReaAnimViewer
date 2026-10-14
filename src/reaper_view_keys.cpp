// SPDX-License-Identifier: MIT
//
// See reaper_view_keys.h.

#include "reaper_view_keys.h"

#include <algorithm>
#include <sstream>

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

}  // namespace rav
