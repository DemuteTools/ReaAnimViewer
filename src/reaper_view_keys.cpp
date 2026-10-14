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

// The gesture of a KEY line (false for an input RAV does not read: MIDI, the Win key, a trackpad...).
bool GestureOfLine(const KbLine& l, KbGesture* out)
{
    KbGesture g;
    if (l.mod == 255) {
        // As ParseViewKeymap: the input in the high nibble, bit 3 clear = Win held.
        if (l.key < 0 || l.key > 255 || (l.key & 8) == 0) return false;
        if ((l.key & 0xF0) == 0xF0) g.input = KbInput::Wheel;
        else if ((l.key & 0xF0) == 0xD0) g.input = KbInput::HWheel;
        else return false;
        g.mods = l.key & 7;
    } else {
        if (l.mod < 0 || l.mod > 127 || l.key <= 0 || (l.mod & 32)) return false;
        g.input = KbInput::Key;
        g.is_char = (l.mod & 1) == 0;
        g.code = l.key;
        g.mods = ((l.mod & 8) ? kViewModCtrl : 0) | ((l.mod & 16) ? kViewModAlt : 0) | ((l.mod & 4) ? kViewModShift : 0);
    }
    *out = g;
    return true;
}

// The lines in force: the defaults the file has not taken over, then the file's. 13-60b: a default
// REAPER itself lists for another action (`listed`) is taken over too (REAPER's word wins over the
// table RAV knows: a REAPER whose defaults differ, or a binding the file does not show).
std::vector<KbLine> LinesInForce(const ViewKeymap& k)
{
    std::vector<KbLine> all;
    for (const KbLine& d : DefaultKbLines()) {
        bool taken = std::any_of(k.lines.begin(), k.lines.end(),
                                 [&d](const KbLine& l) { return l.mod == d.mod && l.key == d.key; });
        KbGesture dg;
        if (!taken && GestureOfLine(d, &dg))
            taken = std::any_of(k.listed.begin(), k.listed.end(), [&d, &dg](const std::pair<std::string, KbGesture>& l) {
                return l.second == dg && !SameCommandId(l.first, d.cmd);
            });
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
        if (!GestureOfLine(l, &g)) continue;
        if (std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
    }
    for (const auto& l : k.listed)
        if (SameCommandId(l.first, command) && std::find(out.begin(), out.end(), l.second) == out.end())
            out.push_back(l.second);
    return out;
}

const std::vector<int>& ViewCommandIds()
{
    static const std::vector<int> kIds = {988, 977, 981, 975, 989, 978, 976, 982, 990, 979, 1000, 1001, 991, 980, 1012, 1011};
    return kIds;
}

void ApplyListedViewGestures(ViewKeymap& k)
{
    for (const auto& l : k.listed) {
        const std::string id = NormalizeCommandId(l.first);
        if (id.empty() || id[0] == '_') continue;
        const int        cmd = std::atoi(id.c_str());
        const KbGesture& g = l.second;
        if (g.input == KbInput::Wheel || g.input == KbInput::HWheel) {
            const ViewWheelBinding b = ViewWheelBindingOf(cmd);
            if (b.action == ViewWheelAction::None) continue;
            (g.input == KbInput::Wheel ? k.wheel : k.hwheel)[g.mods & 7] = b;
        } else if (cmd == 1012 || cmd == 1011) {
            ViewKeyBinding b;
            b.is_char = g.is_char;
            b.code = g.code;
            b.mods = g.mods & 7;
            Remove(cmd == 1012 ? k.zoom_out : k.zoom_in, b);
            Add(cmd == 1012 ? k.zoom_in : k.zoom_out, b);
        }
    }
}

namespace {

// A Latin-1 letter with an accent as its plain lower-case letter (0 for anything else): the key names
// Windows and a LangPack write in capitals with accents ("PAVÉ NUM.", "ENTRÉE").
char FoldLatin1(int cp)
{
    if (cp >= 0xE0) cp -= 0x20;  // lower case -> upper case (the two halves line up)
    if (cp >= 0xC0 && cp <= 0xC5) return 'a';
    if (cp == 0xC7) return 'c';
    if (cp >= 0xC8 && cp <= 0xCB) return 'e';
    if (cp >= 0xCC && cp <= 0xCF) return 'i';
    if (cp == 0xD1) return 'n';
    if ((cp >= 0xD2 && cp <= 0xD6) || cp == 0xD8) return 'o';
    if (cp >= 0xD9 && cp <= 0xDC) return 'u';
    if (cp == 0xDD || cp == 0xDF) return 'y';
    return 0;
}

// Lower case, accents folded, blanks squeezed to one: the form the name tables below are written in.
std::string FoldName(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xC3 && i + 1 < s.size()) {  // U+00C0..U+00FF in UTF-8
            const char f = FoldLatin1(0xC0 + (static_cast<unsigned char>(s[i + 1]) & 0x3F));
            if (f) {
                out += f;
                ++i;
                continue;
            }
        }
        if (c == ' ' || c == '\t') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        out += static_cast<char>(std::tolower(c));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// The code point of a text that is exactly one character (UTF-8, up to U+FFFF), else -1.
int SingleCodePoint(const std::string& s)
{
    if (s.empty()) return -1;
    const unsigned char c = static_cast<unsigned char>(s[0]);
    if (c < 0x80) return s.size() == 1 ? c : -1;
    if ((c & 0xE0) == 0xC0 && s.size() == 2 && (static_cast<unsigned char>(s[1]) & 0xC0) == 0x80)
        return ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[1]) & 0x3F);
    if ((c & 0xF0) == 0xE0 && s.size() == 3 && (static_cast<unsigned char>(s[1]) & 0xC0) == 0x80 &&
        (static_cast<unsigned char>(s[2]) & 0xC0) == 0x80)
        return ((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[1]) & 0x3F) << 6) | (static_cast<unsigned char>(s[2]) & 0x3F);
    return -1;
}

struct NamedKey {
    const char* name;  // FoldName's form
    int         vk;
};

// The names REAPER (English), Windows' layouts (English, French) and the French LangPack give a key.
const NamedKey kNamedKeys[] = {
    {"space", 0x20}, {"spacebar", 0x20}, {"espace", 0x20}, {"barre d'espace", 0x20},
    {"enter", 0x0D}, {"return", 0x0D}, {"entree", 0x0D},
    {"tab", 0x09}, {"tabulation", 0x09},
    {"backspace", 0x08}, {"back", 0x08}, {"ret.arr", 0x08}, {"ret. arr", 0x08}, {"retour arriere", 0x08},
    {"delete", 0x2E}, {"del", 0x2E}, {"suppr", 0x2E}, {"suppr.", 0x2E}, {"supprimer", 0x2E},
    {"insert", 0x2D}, {"ins", 0x2D}, {"inser", 0x2D}, {"inser.", 0x2D}, {"inserer", 0x2D}, {"insertion", 0x2D},
    {"home", 0x24}, {"debut", 0x24}, {"origine", 0x24},
    {"end", 0x23}, {"fin", 0x23},
    {"pageup", 0x21}, {"page up", 0x21}, {"pgup", 0x21}, {"pg up", 0x21}, {"prior", 0x21},
    {"page precedente", 0x21}, {"page prec.", 0x21}, {"page prec", 0x21}, {"pg.prec", 0x21}, {"pg. prec", 0x21},
    {"pg prec", 0x21}, {"pg.prec.", 0x21},
    {"pagedown", 0x22}, {"page down", 0x22}, {"pgdn", 0x22}, {"pg dn", 0x22}, {"page dn", 0x22}, {"next", 0x22},
    {"page suivante", 0x22}, {"page suiv.", 0x22}, {"page suiv", 0x22}, {"pg.suiv", 0x22}, {"pg. suiv", 0x22},
    {"pg suiv", 0x22}, {"pg.suiv.", 0x22},
    {"left", 0x25}, {"gauche", 0x25}, {"up", 0x26}, {"haut", 0x26}, {"right", 0x27}, {"droite", 0x27},
    {"down", 0x28}, {"bas", 0x28},
    {"pause", 0x13}, {"attn", 0x13},
    {"esc", 0x1B}, {"escape", 0x1B}, {"echap", 0x1B}, {"echappement", 0x1B},
    {"help", 0x2F}, {"aide", 0x2F},
    {"apps", 0x5D}, {"application", 0x5D}, {"menu", 0x5D},
};

// A numeric keypad key: "Num +", "NumPad 7", "Num Del", "+ (PAVÉ NUM.)", "PAVÉ NUM. 0" (FoldName's form).
// 0 when the name is no keypad key (Enter on the keypad, which no virtual key tells apart, neither).
int KeypadKey(const std::string& lk)
{
    static const char* const kMarks[] = {"(pave num.)", "(pave num)", "pave num.", "pave num", "(pav. num.)", "pav. num.",
                                         "numpad", "num pad", "keypad", "(num)", "num"};
    for (const char* m : kMarks) {
        const size_t at = lk.find(m);
        if (at == std::string::npos) continue;
        std::string rest = lk.substr(0, at) + lk.substr(at + std::strlen(m));
        size_t a = 0, b = rest.size();
        while (a < b && (rest[a] == ' ' || rest[a] == '(' || rest[a] == ')')) ++a;
        while (b > a && (rest[b - 1] == ' ' || rest[b - 1] == '(' || rest[b - 1] == ')')) --b;
        rest = rest.substr(a, b - a);
        if (rest.size() == 1 && rest[0] >= '0' && rest[0] <= '9') return 0x60 + (rest[0] - '0');
        if (rest == "*") return 0x6A;
        if (rest == "+") return 0x6B;
        if (rest == "-") return 0x6D;
        if (rest == "." || rest == "," || rest == "del" || rest == "suppr" || rest == "suppr.") return 0x6E;
        if (rest == "/") return 0x6F;
        return 0;
    }
    return 0;
}

}  // namespace

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
    // The modifiers, each once, then the key (which may itself be "+": "Ctrl++"). English, plus the
    // names a LangPack may give them (Maj = Shift in French, Strg / Umschalt in German). The Win key
    // (REAPER: "Win+", "Logo+" on Linux) has no modifier of RAV's: such a shortcut is not followed.
    struct Mod {
        const char* word;  // with its "+"
        int         bit;   // -1 = the Win key
    };
    static const Mod kMods[] = {{"ctrl+", kViewModCtrl}, {"control+", kViewModCtrl}, {"strg+", kViewModCtrl},
                                {"alt+", kViewModAlt},   {"shift+", kViewModShift},  {"maj+", kViewModShift},
                                {"umschalt+", kViewModShift}, {"win+", -1}, {"logo+", -1}, {"super+", -1}};
    bool win = false;
    for (bool more = true; more;) {
        more = false;
        for (const Mod& m : kMods) {
            const size_t n = std::strlen(m.word);
            if (s.size() - p <= n || !starts(m.word)) continue;
            if (m.bit < 0) {
                if (win) continue;
                win = true;
            } else {
                if (g.mods & m.bit) continue;  // twice: not a shortcut REAPER writes
                g.mods |= m.bit;
            }
            p += n;
            more = true;
            break;
        }
    }
    const std::string key = s.substr(p);
    if (key.empty() || win) return false;
    // One character: a letter or a digit is its key; any other (",", "=", "ù", "²") is the key typing it.
    const int cp = SingleCodePoint(key);
    if (cp >= 0) {
        if (cp <= 32 || cp == 127 || (cp >= 0x80 && cp < 0xA0)) return false;
        if (cp < 128 && std::isalpha(cp)) g.code = std::toupper(cp);  // a letter's virtual key
        else if (cp < 128 && std::isdigit(cp)) g.code = cp;
        else {
            g.is_char = true;  // the key typing it (the caller's layout); a capital letter as typed: "Ù" -> ù
            g.code = (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) ? cp + 0x20 : cp;
        }
        if (out) *out = g;
        return true;
    }
    const std::string lk = FoldName(key);
    // The wheels (REAPER: "Mousewheel", "HorizWheel"). A trackpad's own gestures (MultiZoom, MultiRotate,
    // MultiHorz, MultiVert) are not followed: Windows sends RAV a trackpad's scrolling as the wheel.
    if (lk == "mousewheel" || lk == "mouse wheel" || lk == "wheel" || lk == "molette" || lk == "molette souris" ||
        lk == "mausrad") {
        g.input = KbInput::Wheel;
        if (out) *out = g;
        return true;
    }
    if (lk == "horizwheel" || lk == "horiz wheel" || lk == "hwheel" || lk == "h-wheel" || lk == "horizontal wheel" ||
        lk == "molette horizontale" || lk == "molette horiz.") {
        g.input = KbInput::HWheel;
        if (out) *out = g;
        return true;
    }
    if ((lk[0] == 'f') && lk.size() <= 3 && lk.size() >= 2 && std::isdigit(static_cast<unsigned char>(lk[1])) &&
        (lk.size() == 2 || std::isdigit(static_cast<unsigned char>(lk[2])))) {
        const int n = std::atoi(lk.c_str() + 1);
        if (n < 1 || n > 24) return false;
        g.code = 0x70 + n - 1;
        if (out) *out = g;
        return true;
    }
    for (const NamedKey& n : kNamedKeys)
        if (lk == n.name) {
            g.code = n.vk;
            if (out) *out = g;
            return true;
        }
    // The locks are no keys of a shortcut ("Num Lock", "VERR.NUM.").
    if (lk.find("lock") != std::string::npos || lk.find("verr") != std::string::npos) return false;
    const int pad = KeypadKey(lk);
    if (pad == 0) return false;
    g.code = pad;
    if (out) *out = g;
    return true;
}

}  // namespace rav
