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

ViewWheelAction ViewWheelActionOf(int command)
{
    switch (command) {
    case 988: return ViewWheelAction::ScrollH;
    case 989: return ViewWheelAction::ScrollV;
    case 990: return ViewWheelAction::ZoomH;
    case 1000: return ViewWheelAction::ZoomV;
    default: return ViewWheelAction::None;
    }
}

ViewKeymap DefaultViewKeymap()
{
    ViewKeymap k;
    k.wheel[0] = ViewWheelAction::ZoomH;
    k.wheel[kViewModCtrl] = ViewWheelAction::ZoomV;
    k.wheel[kViewModAlt] = ViewWheelAction::ScrollH;
    k.wheel[kViewModCtrl | kViewModAlt] = ViewWheelAction::ScrollV;
    k.hwheel[0] = ViewWheelAction::ScrollH;
    k.hwheel[kViewModAlt] = ViewWheelAction::ZoomH;
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
            const ViewWheelAction a = ViewWheelActionOf(cmd);
            if (key >= 248 && key <= 255) k.wheel[key - 248] = a;
            else if (key >= 120 && key <= 127) k.wheel[key - 120] = a;
            else if (key >= 216 && key <= 223) k.hwheel[key - 216] = a;
            else if (key >= 88 && key <= 95) k.hwheel[key - 88] = a;
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
