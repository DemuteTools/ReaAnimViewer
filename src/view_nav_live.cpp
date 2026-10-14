// SPDX-License-Identifier: MIT
//
// See view_nav.h -- the live half: reaper-kb.ini watched (its date and size checked at most once a
// second, read again when REAPER saved it), the user's choices in ExtState (section ReaAnimViewer,
// key viewnav.<id>, written at once, no undo point: a viewer setting like the keys), the recording
// session of the Shortcuts popup, and the one input reader every timeline calls (ViewNavInput).

#include "view_nav.h"

#ifdef _WIN32

#include <imgui.h>
#include <imgui_internal.h>  // SetKeyOwner: a timeline takes the wheel from ImGui's scrolling

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "console_log.h"
#include "reaper_api.h"
#include "shortcuts.h"

namespace rav {
namespace {

constexpr char kExtSection[] = "ReaAnimViewer";

ViewKeymap     g_reaper = DefaultViewKeymap();
ViewNavChoices g_choices{};
ViewNavMap     g_map = ResolveViewNav(g_reaper, g_choices);
bool           g_choices_loaded = false;

// reaper-kb.ini: when it was last checked / read (steady seconds), and what it was then.
bool                            g_kb_known = false;  // read once at least (or found absent)
double                          g_kb_checked = -1.0e9;
double                          g_kb_read_at = -1.0;  // -1: not found (REAPER's defaults)
std::filesystem::file_time_type g_kb_time{};
std::uintmax_t                  g_kb_size = 0;

// Recording.
int         g_rec_row = -1;
bool        g_has_pending = false;
ViewGesture g_pending;
int         g_pending_owner = -1;
std::string g_refusal;

int g_hover_frame = -100;  // a timeline was under the mouse at this frame (ViewNavTakesKey)

double Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::filesystem::path KbPath()
{
    const char* rp = GetResourcePath ? GetResourcePath() : nullptr;
    if (!rp || !rp[0]) return {};
    return std::filesystem::u8path(std::string(rp) + "/reaper-kb.ini");
}

void Rebuild()
{
    g_map = ResolveViewNav(g_reaper, g_choices);
}

std::string ExtKey(int row)
{
    return std::string("viewnav.") + kViewNavTable[row].id;
}

void Save(int row)
{
    if (row < 0 || row >= kNavRowCount) return;
    const std::string key = ExtKey(row);
    const std::string v = EncodeViewNavChoice(g_choices[static_cast<size_t>(row)]);
    if (v.empty()) {
        if (DeleteExtState) DeleteExtState(kExtSection, key.c_str(), true);
    } else if (SetExtState) {
        SetExtState(kExtSection, key.c_str(), v.c_str(), true);
    }
}

void LoadChoices()
{
    g_choices_loaded = true;
    g_choices = ViewNavChoices{};
    for (int row = 0; row < kNavRowCount && GetExtState; ++row) {
        const std::string key = ExtKey(row);
        const char*       v = GetExtState(kExtSection, key.c_str());
        if (!v || !v[0]) continue;
        ViewNavChoice c;
        if (DecodeViewNavChoice(v, &c)) {
            g_choices[static_cast<size_t>(row)] = c;
        } else {
            LogWarn("view action \"%s\": stored value \"%s\" is not valid, linked to REAPER again", kViewNavTable[row].label, v);
            Save(row);  // linked: the stale entry goes
        }
    }
}

// Reads reaper-kb.ini when it changed (or the first time); REAPER's defaults when it is absent.
void PollKb()
{
    const double now = Now();
    if (g_kb_known && now - g_kb_checked < 1.0) return;
    g_kb_checked = now;
    const std::filesystem::path p = KbPath();
    std::error_code ec;
    const bool exists = !p.empty() && std::filesystem::is_regular_file(p, ec);
    std::filesystem::file_time_type t{};
    std::uintmax_t                  size = 0;
    if (exists) {
        t = std::filesystem::last_write_time(p, ec);
        if (!ec) size = std::filesystem::file_size(p, ec);
    }
    const bool changed = !g_kb_known || exists != (g_kb_read_at >= 0.0) || (exists && (t != g_kb_time || size != g_kb_size));
    if (!changed) return;
    g_kb_known = true;
    std::string text;
    if (exists) {
        std::ifstream f(p, std::ios::binary);
        if (f) text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    g_reaper = ParseViewKeymap(text);
    g_kb_time = t;
    g_kb_size = size;
    g_kb_read_at = exists ? now : -1.0;
    Rebuild();
}

void EndRecording()
{
    g_rec_row = -1;
    g_has_pending = false;
    g_pending_owner = -1;
}

void Assign(const ViewGesture& g)
{
    if (g_rec_row < 0) return;
    const ViewNavChoices before = g_choices;
    ViewNavAssign(g_choices, g_rec_row, g);
    for (int row = 0; row < kNavRowCount; ++row)
        if (!(before[static_cast<size_t>(row)] == g_choices[static_cast<size_t>(row)])) Save(row);
    Rebuild();
    EndRecording();
}

void RecordGesture(const ViewGesture& g)
{
    if (g_rec_row < 0) return;
    int owner = -1;
    switch (ViewNavRecordStep(CurrentViewNav(), g_rec_row, g, &owner)) {
    case NavRecordOutcome::Ignored:
        if (g.kind != GestureKind::Key && !kViewNavTable[g_rec_row].wheel_ok)
            g_refusal = std::string(kViewNavTable[g_rec_row].label) + " takes a key, not the wheel.";
        break;
    case NavRecordOutcome::Conflict:
        g_refusal.clear();
        g_has_pending = true;
        g_pending = g;
        g_pending_owner = owner;
        break;
    case NavRecordOutcome::Assigned:
        Assign(g);
        break;
    }
}

int ModsOf(bool ctrl, bool shift, bool alt)
{
    return (ctrl ? kViewModCtrl : 0) | (alt ? kViewModAlt : 0) | (shift ? kViewModShift : 0);
}

ImGuiKey KeyOfVk(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    if (vk >= 0x60 && vk <= 0x69) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - 0x60));
    if (vk >= 0x70 && vk <= 0x87) return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - 0x70));  // F1-F24
    switch (vk) {
    case 0x6B: return ImGuiKey_KeypadAdd;
    case 0x6D: return ImGuiKey_KeypadSubtract;
    case 0x6A: return ImGuiKey_KeypadMultiply;
    case 0x6F: return ImGuiKey_KeypadDivide;
    case 0x6E: return ImGuiKey_KeypadDecimal;
    case 0xBB: return ImGuiKey_Equal;
    case 0xBD: return ImGuiKey_Minus;
    case 0xBC: return ImGuiKey_Comma;
    case 0xBE: return ImGuiKey_Period;
    case 0xBA: return ImGuiKey_Semicolon;
    case 0xBF: return ImGuiKey_Slash;
    case 0xC0: return ImGuiKey_GraveAccent;
    case 0xDB: return ImGuiKey_LeftBracket;
    case 0xDC: return ImGuiKey_Backslash;
    case 0xDD: return ImGuiKey_RightBracket;
    case 0xDE: return ImGuiKey_Apostrophe;
    case 0x20: return ImGuiKey_Space;
    case 0x0D: return ImGuiKey_Enter;
    case 0x09: return ImGuiKey_Tab;
    case 0x08: return ImGuiKey_Backspace;
    case 0x2D: return ImGuiKey_Insert;
    case 0x2E: return ImGuiKey_Delete;
    case 0x21: return ImGuiKey_PageUp;
    case 0x22: return ImGuiKey_PageDown;
    case 0x24: return ImGuiKey_Home;
    case 0x23: return ImGuiKey_End;
    case 0x25: return ImGuiKey_LeftArrow;
    case 0x26: return ImGuiKey_UpArrow;
    case 0x27: return ImGuiKey_RightArrow;
    case 0x28: return ImGuiKey_DownArrow;
    default: return ImGuiKey_None;
    }
}

}  // namespace

const ViewNavMap& CurrentViewNav()
{
    if (!g_choices_loaded) {
        LoadChoices();
        Rebuild();
    }
    PollKb();
    return g_map;
}

const ViewKeymap& CurrentReaperViewKeys()
{
    CurrentViewNav();
    return g_reaper;
}

const ViewNavChoices& CurrentViewNavChoices()
{
    CurrentViewNav();
    return g_choices;
}

double ViewNavKbAge()
{
    CurrentViewNav();
    return g_kb_read_at < 0.0 ? -1.0 : Now() - g_kb_read_at;
}

void SetViewNavLinked(int row)
{
    if (row < 0 || row >= kNavRowCount) return;
    CurrentViewNav();
    g_choices[static_cast<size_t>(row)] = ViewNavChoice{};
    Save(row);
    Rebuild();
}

void SetViewNavOwn(int row)
{
    if (row < 0 || row >= kNavRowCount) return;
    CurrentViewNav();
    if (g_choices[static_cast<size_t>(row)].own) return;
    g_choices[static_cast<size_t>(row)].own = true;  // not set yet: the user records it (double-click)
    g_choices[static_cast<size_t>(row)].g = ViewGesture{};
    Save(row);
    Rebuild();
}

std::string ViewGestureLabel(const ViewGesture& g)
{
    return FormatViewGesture(g, &LayoutKeyName);
}

std::string ViewNavRowText(int row)
{
    std::string s;
    for (const ViewGesture& g : ViewNavGestures(CurrentViewNav(), row)) s += (s.empty() ? "" : ", ") + ViewGestureLabel(g);
    return s.empty() ? std::string("none: see Shortcuts") : s;
}

std::string ViewNavTip()
{
    return ViewNavHint(CurrentViewNav(), &LayoutKeyName);
}

void BeginViewNavRecording(int row)
{
    if (row < 0 || row >= kNavRowCount) return;
    EndRecording();
    g_refusal.clear();
    g_rec_row = row;
}

void CancelViewNavRecording()
{
    EndRecording();
    g_refusal.clear();
}

int ViewNavRecordingRow()
{
    return g_rec_row;
}

void ViewNavRecordKey(unsigned vk, bool ctrl, bool shift, bool alt)
{
    if (g_rec_row < 0) return;
    if (vk == vk::kEscape) {
        CancelViewNavRecording();
        return;
    }
    if (!IsBindableKey(vk)) return;  // a modifier on its own: still waiting
    // A key of RAV's own table (V, C, Delete...) stays its own: refused, still waiting.
    const int rav = ShortcutOwner(CurrentShortcuts(), KeyBinding{vk, ctrl, shift, alt}, -1);
    if (rav >= 0) {
        g_has_pending = false;
        g_refusal = std::string("That key is RAV's \"") + kShortcutTable[rav].label + "\": pick another.";
        return;
    }
    g_refusal.clear();
    RecordGesture(KeyGesture(static_cast<int>(vk), ModsOf(ctrl, shift, alt)));
}

void ViewNavRecordWheel(bool horizontal, bool ctrl, bool shift, bool alt)
{
    if (g_rec_row < 0) return;
    g_refusal.clear();
    RecordGesture(WheelGesture(horizontal, ModsOf(ctrl, shift, alt)));
}

bool ViewNavPending(ViewGesture* g, int* owner)
{
    if (g_rec_row < 0 || !g_has_pending) return false;
    if (g) *g = g_pending;
    if (owner) *owner = g_pending_owner;
    return true;
}

void ViewNavKeepPending()
{
    if (g_rec_row >= 0 && g_has_pending) Assign(g_pending);
}

const char* ViewNavRefusal()
{
    return g_refusal.c_str();
}

ViewNavStep ViewNavInput(bool wheel_on, bool keys_on, unsigned owner_id)
{
    const ViewNavMap& m = CurrentViewNav();
    if (!ImGui::GetCurrentContext()) return ViewNavStep{};
    ImGuiIO&  io = ImGui::GetIO();
    const int mods = ModsOf(io.KeyCtrl, io.KeyShift, io.KeyAlt);
    if (wheel_on) {
        g_hover_frame = ImGui::GetFrameCount();
        // The wheel this view runs is not ImGui's to scroll a window with (from the next frame: the
        // owner is read when the frame starts; the view stays under the mouse meanwhile).
        if (owner_id != 0) {
            if (ViewNavWheelBound(m, mods, false)) ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, owner_id);
            if (ViewNavWheelBound(m, mods, true)) ImGui::SetKeyOwner(ImGuiKey_MouseWheelX, owner_id);
        }
    }
    std::vector<int> rows;
    if (keys_on && !io.WantTextInput && ShortcutRecordingId() < 0) {
        for (const auto& k : m.keys) {
            bool hit = false;
            if (k.first.is_char) {
                for (int i = 0; i < io.InputQueueCharacters.Size && !hit; ++i)
                    hit = static_cast<int>(io.InputQueueCharacters[i]) == k.first.code;
            } else if (k.first.mods == mods) {
                const ImGuiKey key = KeyOfVk(k.first.code);
                hit = key != ImGuiKey_None && ImGui::IsKeyPressed(key, true);
            }
            if (hit) rows.push_back(k.second);
        }
    }
    return ViewNavStepFor(m, mods, wheel_on ? io.MouseWheel : 0.0f, wheel_on ? io.MouseWheelH : 0.0f, rows);
}

bool ViewNavTakesKey(unsigned vk, bool ctrl, bool shift, bool alt)
{
    if (!ImGui::GetCurrentContext() || g_hover_frame < ImGui::GetFrameCount() - 1) return false;
    return ViewNavKeyRow(CurrentViewNav(), static_cast<int>(vk), ModsOf(ctrl, shift, alt)) >= 0;
}

}  // namespace rav

#endif  // _WIN32
