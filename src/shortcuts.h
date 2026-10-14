// SPDX-License-Identifier: MIT
//
// The viewer's keyboard shortcuts (spec 11-fb-3): one table (id, label, default key,
// context) drives the key handling, the accelerator hook, the Shortcuts popup and the
// key caps drawn in the Video view. A binding is one virtual key plus optional
// Ctrl / Shift / Alt, and it matches only with exactly those modifiers held, so every
// other key (Space, ...) stays REAPER's.
//
// The top half is pure (no REAPER, no Windows): the table, the matching rule, the
// record / conflict / reset rules, the ExtState encoding and the key names. The host
// tests (tests/shortcuts_test.cpp) include it as is. The bottom half is the live state
// (src/shortcuts.cpp): the current bindings, REAPER ExtState, the recording session.

#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rav {

// ---- virtual keys (the Windows VK_* values, spelled here so this half stays Windows-free)

namespace vk {
constexpr unsigned kBack = 0x08, kTab = 0x09, kReturn = 0x0D, kShift = 0x10, kControl = 0x11,
                   kMenu = 0x12, kPause = 0x13, kCapital = 0x14, kEscape = 0x1B, kSpace = 0x20,
                   kPrior = 0x21, kNext = 0x22, kEnd = 0x23, kHome = 0x24, kLeft = 0x25, kUp = 0x26,
                   kRight = 0x27, kDown = 0x28, kInsert = 0x2D, kDelete = 0x2E, kLWin = 0x5B,
                   kRWin = 0x5C, kApps = 0x5D, kNumpad0 = 0x60, kMultiply = 0x6A, kAdd = 0x6B,
                   kSeparator = 0x6C, kSubtract = 0x6D, kDecimal = 0x6E, kDivide = 0x6F, kF1 = 0x70,
                   kF11 = 0x7A, kF24 = 0x87, kNumLock = 0x90, kScroll = 0x91, kLShift = 0xA0, kRMenu = 0xA5,
                   kProcessKey = 0xE5, kPacket = 0xE7;
}  // namespace vk

// ---- the table -----------------------------------------------------------------------

enum class ShortcutContext {
    Anywhere,     // the viewer has the focus
    VideoView,    // ...and Video view is on (in RAV view the key stays REAPER's)
    TaggingView,  // ...and Tagging view is on (story 10-4)
    LibraryEdit,  // 13-61: ...and the Library's Sound Edit window has the focus
};

// Two contexts can be on at once (then their actions cannot share a key): Anywhere overlaps
// every context; Video view, Tagging view and the Library's Edit window never are on together.
inline bool ShortcutContextsOverlap(ShortcutContext a, ShortcutContext b)
{
    return a == ShortcutContext::Anywhere || b == ShortcutContext::Anywhere || a == b;
}

struct KeyBinding {
    unsigned vk = 0;  // Windows virtual-key code (0 = none)
    bool ctrl  = false;
    bool shift = false;
    bool alt   = false;
};

inline bool operator==(const KeyBinding& a, const KeyBinding& b)
{
    return a.vk == b.vk && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt;
}
inline bool operator!=(const KeyBinding& a, const KeyBinding& b) { return !(a == b); }

struct ShortcutDef {
    const char*     id;     // ExtState key suffix: shortcut.<id>
    const char*     label;  // shown in the popup and in "Already used by <label>"
    KeyBinding      def;    // the default key
    ShortcutContext context;
    // Lot 13-60b: the action RAV registers in REAPER's action list for it (Main section): its name
    // (REAPER's id is "_" + it) and the text REAPER lists it by. A row on "REAPER" runs on the keys the
    // user gives that action in REAPER's Actions window; REAPER runs it too (outside the viewer).
    const char*     action;
    const char*     action_desc;
    // 13-61: a row whose REAPER mode follows one of REAPER's own actions (its command id, "40511")
    // instead of a RAV action: it has none of its own (action / action_desc null), its REAPER mode is
    // its default (no key of its own: `def` is none), and REAPER's keys for that action run it in RAV
    // while its context is on (in REAPER's main window they still run REAPER's action).
    const char*     reaper_cmd = nullptr;
    const char*     reaper_name = nullptr;  // ...and REAPER's name of it, for the tips
    // 13-63: a REAPER-default row's fallback key: on REAPER, it runs the row while REAPER lists no key for
    // reaper_cmd (live: once REAPER lists one, that key takes over and this one is free again); Own starts
    // on it (when free). vk 0 = none (the trims: no key at all while REAPER has none).
    KeyBinding      reaper_fallback = {};
};

enum ShortcutId : int {
    kShortcutToggleView  = 0,  // RAV view / Video view
    kShortcutTogglePanel = 1,  // the Video panel (from RAV view it opens Video view with it)
    kShortcutCut         = 2,  // cut at the playhead (Video view only)
    kShortcutDeleteShot  = 3,  // delete the current shot, after a confirmation (Video view only, spec 11-fb-11)
    kShortcutCopyCamera  = 4,  // copy the current shot's camera (Video view only)
    kShortcutPasteCamera = 5,  // paste it into the current shot (Video view only)
    kShortcutTagAddEvent = 6,  // story 10-4: add an event at the playhead on the selected rule (Tagging view only)
    kShortcutTagDelEvent = 7,  // story 10-4: suppress / restore / delete the selected event (Tagging view only)
    kShortcutFullScreen  = 8,  // story 6-5-9: the viewer full screen on / off (Esc leaves it too)
    kShortcutClipList    = 9,  // story 6-5-10: the animation clip list open / closed (every view)
    // 13-61: the Sound Edit window's trims (Library Edit only), on REAPER's 40511 / 40512 / 40508 keys.
    kShortcutEditTrimLeft  = 10,  // the variation under the edit cursor starts at the cursor
    kShortcutEditTrimRight = 11,  // ...ends at the cursor
    kShortcutEditTrimZone  = 12,  // the selected variation(s) trimmed to the zone selected
    // 13-63: the Sound Edit window's split, on REAPER's 40757 keys (B while REAPER gives 40757 none).
    kShortcutEditSplit     = 13,  // the variation(s) under the edit cursor split there
    // 13-62: the Sound Edit window's listening, on REAPER's 40281 / 40280 keys (no key while REAPER has none).
    kShortcutEditSolo      = 14,  // the selected part soloed / unsoloed (view state, never saved)
    kShortcutEditMute      = 15,  // ...muted / unmuted
    // 13-62F (F5): chain editing, on REAPER's 40417 / 40416 keys (no key while REAPER has none).
    kShortcutEditNextVar   = 16,  // the next variation selected, the cursor at its start, the view centred on it
    kShortcutEditPrevVar   = 17,  // ...the previous one
    kShortcutCount       = 18,
};

inline const ShortcutDef kShortcutTable[kShortcutCount] = {
    {"toggle_view",  "Next view: RAV / Tagging / Video", {'V', false, false, false}, ShortcutContext::Anywhere,
     "RAV_KEY_NEXT_VIEW", "RAV: Viewer key: Next view (RAV / Tagging / Video)"},
    {"toggle_panel", "Show / hide the Video panel",  {'P', false, false, false}, ShortcutContext::Anywhere,
     "RAV_KEY_VIDEO_PANEL", "RAV: Viewer key: Show / hide the Video panel"},
    {"cut",          "Cut at playhead",              {'C', false, false, false}, ShortcutContext::VideoView,
     "RAV_KEY_VIDEO_CUT", "RAV: Viewer key: Cut at playhead (Video view)"},
    {"delete_shot",  "Delete current shot",          {vk::kDelete, false, false, false}, ShortcutContext::VideoView,
     "RAV_KEY_VIDEO_DELETE_SHOT", "RAV: Viewer key: Delete current shot (Video view)"},
    {"copy_camera",  "Copy shot camera",             {'C', true, false, false}, ShortcutContext::VideoView,
     "RAV_KEY_VIDEO_COPY_CAMERA", "RAV: Viewer key: Copy shot camera (Video view)"},
    {"paste_camera", "Paste shot camera",            {'V', true, false, false}, ShortcutContext::VideoView,
     "RAV_KEY_VIDEO_PASTE_CAMERA", "RAV: Viewer key: Paste shot camera (Video view)"},
    {"tag_add_event", "Add event at playhead",       {'E', false, false, false}, ShortcutContext::TaggingView,
     "RAV_KEY_TAG_ADD_EVENT", "RAV: Viewer key: Add event at playhead (Tagging view)"},
    {"tag_del_event", "Suppress / delete event",     {vk::kDelete, false, false, false}, ShortcutContext::TaggingView,
     "RAV_KEY_TAG_DELETE_EVENT", "RAV: Viewer key: Suppress / delete event (Tagging view)"},
    // Story 6-5-9's own action (plugin_main.cpp registers it, with REAPER's toolbar toggle state).
    {"full_screen",  "Full screen on / off",         {vk::kF11, false, false, false}, ShortcutContext::Anywhere,
     "RAV_VIEWER_FULLSCREEN", "RAV: Viewer full screen (toggle)"},
    {"clip_list",    "Clip list: open / close",      {'L', false, false, false}, ShortcutContext::Anywhere,
     "RAV_KEY_CLIP_LIST", "RAV: Viewer key: Clip list open / close"},
    // 13-61 (Antho 2026-10-14): REAPER's item trims, on the Sound Edit window's variations.
    {"edit_trim_left", "Trim left of cursor", {0, false, false, false}, ShortcutContext::LibraryEdit, nullptr, nullptr,
     "40511", "Item: Trim items left of cursor"},
    {"edit_trim_right", "Trim right of cursor", {0, false, false, false}, ShortcutContext::LibraryEdit, nullptr, nullptr,
     "40512", "Item: Trim items right of cursor"},
    {"edit_trim_zone", "Trim to selected zone", {0, false, false, false}, ShortcutContext::LibraryEdit, nullptr, nullptr,
     "40508", "Item: Trim items to selected area"},
    // 13-63 (Antho 2026-10-14): REAPER's split at the edit cursor; B when REAPER gives 40757 no key.
    {"edit_split", "Split at cursor", {0, false, false, false}, ShortcutContext::LibraryEdit, nullptr, nullptr,
     "40757", "Item: Split items at edit cursor (no change selection)", {'B', false, false, false}},
    // 13-62 (decision 12, ids checked in REAPER 7.81): REAPER's track solo / mute toggles, on the selected part.
    {"edit_solo_part", "Solo selected part", {0, false, false, false}, ShortcutContext::LibraryEdit, nullptr, nullptr,
     "40281", "Track: Solo/unsolo tracks"},
    {"edit_mute_part", "Mute selected part", {0, false, false, false}, ShortcutContext::LibraryEdit, nullptr, nullptr,
     "40280", "Track: Mute/unmute tracks"},
    // 13-62F (F5, Antho 2026-10-14): REAPER's item navigation, to chain through the variations at a fixed zoom.
    {"edit_next_variation", "Next variation", {0, false, false, false}, ShortcutContext::LibraryEdit, nullptr, nullptr,
     "40417", "Item navigation: Select and move to next item"},
    {"edit_prev_variation", "Previous variation", {0, false, false, false}, ShortcutContext::LibraryEdit, nullptr, nullptr,
     "40416", "Item navigation: Select and move to previous item"},
};

// 13-60b: REAPER's id of a row's own action ("_RAV_KEY_VIDEO_CUT"), "" out of range. 13-61: a row on one
// of REAPER's own actions, its id ("40511").
inline std::string ShortcutReaperActionId(int id)
{
    if (id < 0 || id >= kShortcutCount) return std::string();
    if (kShortcutTable[id].reaper_cmd) return kShortcutTable[id].reaper_cmd;
    return kShortcutTable[id].action ? std::string("_") + kShortcutTable[id].action : std::string();
}

// 13-61: a row whose default is REAPER (it follows one of REAPER's own actions, ShortcutDef::reaper_cmd).
inline bool ShortcutDefaultFollowsReaper(int id)
{
    return id >= 0 && id < kShortcutCount && kShortcutTable[id].reaper_cmd != nullptr;
}

// 13-63: the key a REAPER-default row runs on while REAPER lists none for its action (vk 0 = none), and
// the key it starts on when made Own. Other rows: none.
inline KeyBinding ShortcutReaperFallback(int id)
{
    return ShortcutDefaultFollowsReaper(id) ? kShortcutTable[id].reaper_fallback : KeyBinding{};
}

// 13-60b: a row on "REAPER" (following its own action's keys in REAPER) is stored as this.
constexpr char kShortcutFollowsReaper[] = "reaper";
// 13-61: a row whose default is REAPER, set to Own with no key yet, is stored as this.
constexpr char kShortcutOwnNoKey[] = "own";

// The mouse gestures the popup lists (fixed, never rebindable), by group.
struct MouseGestureGroup {
    const char* title;   // "MOUSE"
    const char* note;    // faint, next to the title (or nullptr)
    const char* footer;  // faint, under the group's rows (or nullptr)
};

inline const MouseGestureGroup kMouseGestureGroups[] = {
    {"MOUSE", nullptr, "In Video view these gestures edit the shot under the playhead."},
    {"SHOT STRIP", "Video view", nullptr},
};

struct MouseGestureDef {
    int         group;    // index in kMouseGestureGroups
    const char* action;
    const char* gesture;
};

inline const MouseGestureDef kMouseGestureTable[] = {
    {0, "Orbit", "Right-drag"},
    {0, "Pan", "Middle-drag"},
    {0, "Hand scroll (Sound Edit, Blend lanes)", "Middle-drag"},  // 13-45 (the mockup: fixed)
    {0, "Zoom", "Wheel"},
    {0, "Menu, buttons, nav cube", "Left-click"},
    {1, "Move the playhead", "Click / drag"},
    {1, "Retime a cut", "Drag a line between shots"},
    {1, "Switch Cut / Move", "Middle-click a shot"},
    {1, "Resize the strip", "Drag its top edge"},
};

using ShortcutBindings = std::array<KeyBinding, kShortcutCount>;

inline ShortcutBindings DefaultShortcutBindings()
{
    ShortcutBindings b;
    for (int i = 0; i < kShortcutCount; ++i) b[static_cast<size_t>(i)] = kShortcutTable[i].def;
    return b;
}


// A key that only modifies others (or toggles a lock): never recorded on its own.
inline bool IsModifierKey(unsigned k)
{
    return k == vk::kShift || k == vk::kControl || k == vk::kMenu || (k >= vk::kLShift && k <= vk::kRMenu) ||
           k == vk::kLWin || k == vk::kRWin || k == vk::kCapital || k == vk::kNumLock || k == vk::kScroll;
}

// Can this key be a binding? Not a mouse button, not a modifier, not Esc (it cancels).
inline bool IsBindableKey(unsigned k)
{
    if (k == 0 || k > 0xFE) return false;  // 0xFF: no key (a layout's "none")
    if (k == 0x01 || k == 0x02 || k == 0x04 || k == 0x05 || k == 0x06) return false;  // mouse buttons
    if (k == 0x03) return false;  // VK_CANCEL (Ctrl+Break)
    if (k == vk::kProcessKey || k == vk::kPacket) return false;  // IME / injected Unicode, not a key
    if (k == vk::kEscape) return false;
    return !IsModifierKey(k);
}

// ---- matching ------------------------------------------------------------------------

// True when vk, with exactly these modifiers held, is the binding.
inline bool ShortcutMatches(const KeyBinding& b, unsigned key, bool ctrl, bool shift, bool alt)
{
    return b.vk != 0 && b.vk == key && b.ctrl == ctrl && b.shift == shift && b.alt == alt;
}

// Whether a row's context is on (13-61: `library_edit` = the Library's Sound Edit window has the focus).
inline bool ShortcutContextOn(ShortcutContext c, bool video_view, bool tagging_view, bool library_edit)
{
    if (c == ShortcutContext::VideoView) return video_view;
    if (c == ShortcutContext::TaggingView) return tagging_view;
    if (c == ShortcutContext::LibraryEdit) return library_edit;
    return true;
}

// The action this key press runs, or -1. An action whose context is Video view only
// matches in Video view; Tagging view only, in Tagging view; 13-61: Library Edit only, while the
// Sound Edit window has the focus.
inline int ShortcutActionFor(const ShortcutBindings& b, unsigned key, bool ctrl, bool shift, bool alt,
                             bool video_view, bool tagging_view = false, bool library_edit = false)
{
    for (int i = 0; i < kShortcutCount; ++i) {
        if (!ShortcutContextOn(kShortcutTable[i].context, video_view, tagging_view, library_edit)) continue;
        if (ShortcutMatches(b[static_cast<size_t>(i)], key, ctrl, shift, alt)) return i;
    }
    return -1;
}

// The action other than `except` bound to this key whose context can be on together with
// `except`'s (any context when `except` < 0), or -1. Video view's Delete and Tagging view's
// Delete never clash.
inline int ShortcutOwner(const ShortcutBindings& b, const KeyBinding& key, int except)
{
    if (key.vk == 0) return -1;  // 13-61: no key (a row without one) is nobody's
    for (int i = 0; i < kShortcutCount; ++i) {
        if (i == except || !(b[static_cast<size_t>(i)] == key)) continue;
        if (except >= 0 && except < kShortcutCount &&
            !ShortcutContextsOverlap(kShortcutTable[i].context, kShortcutTable[except].context))
            continue;
        return i;
    }
    return -1;
}

// ---- lot 13-60: rows linked to a REAPER action -------------------------------------------
//
// A row can follow a REAPER action the user picks instead of its own key (action_link.h): it then
// runs on every key REAPER gives that action (reaper-kb.ini, live; the wheel is not a key action's).
// Such a row holds no key of its own in ShortcutBindings (vk 0, never given its default back: the
// `linked` flags below). The keys in force (ShortcutKeymap) are each row's own key, or its action's
// keys; a key the user gave a row in RAV wins over a linked action's, and of two linked rows sharing a
// key (in contexts that can be on together) the first one in the table takes it. The others are
// "shadowed": shown, but they run nothing.

using ShortcutKeyLists = std::array<std::vector<KeyBinding>, kShortcutCount>;

struct ShortcutKeymap {
    ShortcutKeyLists keys;      // what runs each row
    ShortcutKeyLists shadowed;  // a linked row's keys another row takes
};

// The keys in force. `linked[i]` (optional): row i follows `linked_keys[i]` (its action's keys).
inline ShortcutKeymap ResolveShortcutKeys(const ShortcutBindings& own, const ShortcutKeyLists& linked_keys,
                                          const bool* linked)
{
    ShortcutKeymap m;
    for (int i = 0; i < kShortcutCount; ++i)
        if (!(linked && linked[i]) && own[static_cast<size_t>(i)].vk != 0)
            m.keys[static_cast<size_t>(i)].push_back(own[static_cast<size_t>(i)]);
    for (int i = 0; i < kShortcutCount; ++i) {
        if (!(linked && linked[i])) continue;
        for (const KeyBinding& k : linked_keys[static_cast<size_t>(i)]) {
            if (!IsBindableKey(k.vk)) continue;
            std::vector<KeyBinding>& mine = m.keys[static_cast<size_t>(i)];
            if (std::find(mine.begin(), mine.end(), k) != mine.end()) continue;
            bool taken = false;
            for (int j = 0; j < kShortcutCount && !taken; ++j) {
                if (j == i || !ShortcutContextsOverlap(kShortcutTable[j].context, kShortcutTable[i].context)) continue;
                const std::vector<KeyBinding>& other = m.keys[static_cast<size_t>(j)];
                // An own key wins; of two linked rows, the earlier one (already resolved).
                if (std::find(other.begin(), other.end(), k) != other.end() && (!(linked && linked[j]) || j < i))
                    taken = true;
            }
            (taken ? m.shadowed[static_cast<size_t>(i)] : mine).push_back(k);
        }
    }
    return m;
}

// Every row on its own key (no link): the keymap of plain bindings.
inline ShortcutKeymap ShortcutKeymapOf(const ShortcutBindings& b)
{
    return ResolveShortcutKeys(b, ShortcutKeyLists{}, nullptr);
}

// 13-63: `linked_keys` (the keys REAPER gives each followed action) with each fallback put in: a row on
// REAPER (`on_reaper[i]`) whose own REAPER action has no key gets its fallback key. `used` (optional): the
// rows that run on their fallback now. The fallback then resolves as a linked key (ResolveShortcutKeys):
// a row's own key on it wins (the fallback is shown shadowed).
inline void ApplyReaperFallbacks(ShortcutKeyLists& linked_keys, const bool* on_reaper, bool* used = nullptr)
{
    for (int i = 0; i < kShortcutCount; ++i) {
        if (used) used[i] = false;
        const KeyBinding fb = ShortcutReaperFallback(i);
        if (!on_reaper || !on_reaper[i] || fb.vk == 0 || !linked_keys[static_cast<size_t>(i)].empty()) continue;
        linked_keys[static_cast<size_t>(i)].push_back(fb);
        if (used) used[i] = true;
    }
}

// ShortcutActionFor over the keys in force.
inline int ShortcutActionFor(const ShortcutKeymap& m, unsigned key, bool ctrl, bool shift, bool alt, bool video_view,
                             bool tagging_view = false, bool library_edit = false)
{
    for (int i = 0; i < kShortcutCount; ++i) {
        if (!ShortcutContextOn(kShortcutTable[i].context, video_view, tagging_view, library_edit)) continue;
        for (const KeyBinding& k : m.keys[static_cast<size_t>(i)])
            if (ShortcutMatches(k, key, ctrl, shift, alt)) return i;
    }
    return -1;
}

// ShortcutOwner over the keys in force (`except` < 0: any row, any context).
inline int ShortcutKeyOwner(const ShortcutKeymap& m, const KeyBinding& key, int except)
{
    for (int i = 0; i < kShortcutCount; ++i) {
        if (i == except) continue;
        const std::vector<KeyBinding>& ks = m.keys[static_cast<size_t>(i)];
        if (std::find(ks.begin(), ks.end(), key) == ks.end()) continue;
        if (except >= 0 && except < kShortcutCount &&
            !ShortcutContextsOverlap(kShortcutTable[i].context, kShortcutTable[except].context))
            continue;
        return i;
    }
    return -1;
}

// ---- key routing (the accelerator hook's decision) -------------------------------------

// What the hook says to REAPER: keep the key (0), or hand it to the viewer window (-1;
// -20 for WM_SYSKEY* / WM_SYSCHAR, which REAPER would otherwise drop: the Alt keys).
// Story 6-5-9: -666 forces a key to REAPER's main shortcuts: the full screen viewer is a
// top-level window of its own, not a docker child, so REAPER would not take it as its own.
constexpr int kRouteReaper = 0, kRouteViewer = -1, kRouteViewerSys = -20, kRouteReaperMain = -666;

enum class KeyMsg { KeyDown, SysKeyDown, KeyUp, SysKeyUp, Char, SysChar, DeadChar, SysDeadChar, Wheel, Other };

inline bool IsSysKeyMsg(KeyMsg m)
{
    return m == KeyMsg::SysKeyDown || m == KeyMsg::SysKeyUp || m == KeyMsg::SysChar || m == KeyMsg::SysDeadChar;
}

struct KeyRouteInput {
    KeyMsg   msg = KeyMsg::Other;
    unsigned key = 0;         // wParam (the virtual key for key down / up)
    bool     repeat = false;  // key down auto-repeat (lParam bit 30)
    bool     ctrl = false, shift = false, alt = false;
    bool     video_view = false;
    bool     tagging_view = false;    // story 10-4: E / Del in Tagging view
    bool     library_edit = false;    // 13-61: the Library's Sound Edit window has the focus (its trims)
    bool     recording = false;   // a new key is being recorded: the viewer takes every key
    bool     text_input = false;  // an ImGui text field is active: the viewer takes every key
    bool     popup_open = false;  // the Shortcuts popup (or the delete confirmation) is open: Esc closes it
    bool     confirm_open = false;  // the delete confirmation is open: Enter confirms it (spec 11-fb-11)
    bool     imgui_mouse = false;   // the viewer's UI wants the mouse (spec 11-fb-12: Alt + wheel zooms the strip)
    bool     full_screen = false;   // story 6-5-9: Esc is the viewer's (it leaves), other keys go to REAPER's main shortcuts
    bool     horizontal = false;    // 13-45: the wheel message is the horizontal wheel (WM_MOUSEHWHEEL)
    bool     view_wheel = false;    // 13-45: a RAV timeline under the mouse runs this wheel (view_nav.h)
};

struct KeyRouteState {
    unsigned claimed_vk = 0;   // the key the viewer took: its characters and release follow it
    unsigned released_vk = 0;  // its WM_SYSKEYUP was sent to the viewer: the window swallows it
    bool     alt_wheel_taken = false;  // spec 11-fb-12: Alt + wheel went to the UI since Alt went down
    // 13-7: the keys pressed while a text field was active, not yet released. The press that
    // ends the field (Enter, Esc) is released once the field is gone: its release must still
    // reach the viewer, or ImGui keeps the key down and its auto-repeat validates (Enter) or
    // cancels (Esc) the next text field the moment it opens.
    std::array<bool, 256> text_keys{};
};

// Which keys reach the viewer, and how the claim follows a taken key to its release.
// 13-60: `b` is the keys in force (own keys and linked actions' keys, ShortcutKeymap).
inline int RouteViewerKey(const ShortcutKeymap& b, const KeyRouteInput& in, KeyRouteState& st)
{
    const int to_viewer = IsSysKeyMsg(in.msg) ? kRouteViewerSys : kRouteViewer;
    const bool down = in.msg == KeyMsg::KeyDown || in.msg == KeyMsg::SysKeyDown;
    const bool up = in.msg == KeyMsg::KeyUp || in.msg == KeyMsg::SysKeyUp;
    // Spec 11-fb-12 -- Alt + wheel over the viewer's UI (the shot strip's zoom) is the viewer's,
    // never a REAPER mouse-wheel action, and Alt's release after it is the viewer's too (the
    // window swallows it: REAPER's menu bar does not open). Alt's next press starts afresh.
    // Spec 11-fb-14: Shift + wheel over the UI (the strip's scroll) is the viewer's too (Shift's
    // release opens no menu: nothing to swallow).
    // 13-45: KeyMsg::Wheel covers the horizontal wheel too. A wheel a RAV timeline under the mouse
    // runs (the user's REAPER view bindings, src/view_nav.h) is the viewer's, whatever its modifiers:
    // REAPER must not run it on its arrange view instead. Over the UI, the plain and Shift wheel are
    // the viewer's too (Dear ImGui scrolls the panel under the mouse with them). Any other wheel (a
    // Ctrl+wheel over a panel RAV does not use it on) goes on to REAPER, as before. The horizontal
    // wheel without Ctrl is the viewer's anywhere (over the 3D view it does nothing): it must not
    // scroll REAPER's arrange behind.
    if (in.msg == KeyMsg::Wheel) {
        if (in.alt && in.imgui_mouse) {
            st.alt_wheel_taken = true;
            return kRouteViewer;
        }
        if (in.view_wheel) return kRouteViewer;
        if (in.imgui_mouse && !in.ctrl) return kRouteViewer;
        if (in.horizontal && !in.ctrl) return kRouteViewer;
        return (in.recording || in.text_input) ? kRouteViewer : kRouteReaper;
    }
    if (down && in.key == vk::kMenu && !in.repeat) st.alt_wheel_taken = false;
    if (up && in.key == vk::kMenu && st.alt_wheel_taken && !in.recording) {
        st.alt_wheel_taken = false;
        if (in.msg == KeyMsg::SysKeyUp) {
            st.released_vk = vk::kMenu;
            return kRouteViewerSys;
        }
        return kRouteViewer;
    }
    if (in.recording) {
        // The record modal takes every key; the one it records keeps its character and its
        // release once recording ends.
        if (down) st.claimed_vk = in.key;
        if (in.msg == KeyMsg::SysKeyUp) st.released_vk = in.key;
        return to_viewer;
    }
    // Spec 11-fb-17 -- an active text field takes every key, with its characters and release:
    // Ctrl (and Ctrl+A / C / V / X / Z), Home / End, Shift + arrows and the rest of the editing
    // keys, which ImGui's InputText handles itself; Space types a space instead of playing.
    const bool tracked = in.key < st.text_keys.size();
    if (in.text_input) {
        if (tracked && down) st.text_keys[in.key] = true;
        if (tracked && up) st.text_keys[in.key] = false;
        return to_viewer;
    }
    // 13-7: the release (and auto-repeat) of a key pressed in a text field that has closed since.
    if (tracked && st.text_keys[in.key]) {
        if (up) {
            st.text_keys[in.key] = false;
            if (st.claimed_vk == in.key) st.claimed_vk = 0;
            if (in.msg == KeyMsg::SysKeyUp) st.released_vk = in.key;
            return to_viewer;
        }
        if (down && in.repeat) return to_viewer;
        if (down) st.text_keys[in.key] = false;  // a new press: its release was lost (focus moved)
    }
    // Story 6-5-9 -- full screen, a key the viewer does not take goes to REAPER's main shortcuts
    // (Space plays). Not an Alt one: REAPER's menu bar would open behind the full screen window.
    const int to_reaper =
        (in.full_screen && !IsSysKeyMsg(in.msg) && !in.alt) ? kRouteReaperMain : kRouteReaper;
    if (down) {
        st.released_vk = 0;
        // An auto-repeat of the key we took stays ours, even if it no longer maps to an
        // action (e.g. the key just recorded for a Video view only action, in RAV view).
        if (in.repeat && st.claimed_vk != 0 && in.key == st.claimed_vk) return to_viewer;
        // Story 6-5-9: full screen, Esc is the viewer's (it leaves full screen).
        const bool ours = (in.key == vk::kEscape && (in.popup_open || in.full_screen)) ||
                          (in.key == vk::kReturn && in.confirm_open) ||
                          ShortcutActionFor(b, in.key, in.ctrl, in.shift, in.alt, in.video_view, in.tagging_view,
                                            in.library_edit) >= 0;
        if (!ours) {
            st.claimed_vk = 0;  // a lost release (focus moved) must not keep claiming characters
            return to_reaper;
        }
        st.claimed_vk = in.key;
        return to_viewer;
    }
    if (up) {
        // The release of the key we took (its modifiers may already be up) follows it.
        if (st.claimed_vk == 0 || in.key != st.claimed_vk) return to_reaper;
        st.claimed_vk = 0;
        if (in.msg == KeyMsg::SysKeyUp) st.released_vk = in.key;
        return to_viewer;
    }
    if (in.msg == KeyMsg::Char || in.msg == KeyMsg::SysChar || in.msg == KeyMsg::DeadChar ||
        in.msg == KeyMsg::SysDeadChar)
        return st.claimed_vk != 0 ? to_viewer : kRouteReaper;  // the character of the key we took
    return kRouteReaper;
}

// The same with each row on its own key (no linked action).
inline int RouteViewerKey(const ShortcutBindings& b, const KeyRouteInput& in, KeyRouteState& st)
{
    return RouteViewerKey(ShortcutKeymapOf(b), in, st);
}

// Spec 11-fb-17 -- whether a WM_CHAR / WM_SYSCHAR / WM_DEADCHAR reaching the viewer window is
// the viewer's: the character of a key it took (claimed_vk, still set until that key's
// release), any character while a new key is recorded, or while an ImGui text field is
// active. The window then answers it itself (ImGui already has the character) and never lets
// it reach DefWindowProc; other characters keep their old path.
inline bool ViewerTakesChar(const KeyRouteState& st, bool recording, bool text_input)
{
    return recording || text_input || st.claimed_vk != 0;
}

// ---- recording -----------------------------------------------------------------------

enum class RecordOutcome {
    Ignored,    // a modifier on its own (or an unbindable key): still waiting
    Cancelled,  // Esc: the binding is unchanged
    Conflict,   // the key belongs to another action: refused, still waiting
    Assigned,   // the key is now the action's binding
};

// One key down while recording action `id`. On Conflict, *owner is the other action.
inline RecordOutcome ShortcutRecordStep(ShortcutBindings& b, int id, unsigned key, bool ctrl, bool shift,
                                        bool alt, int* owner = nullptr)
{
    if (owner) *owner = -1;
    if (key == vk::kEscape) return RecordOutcome::Cancelled;
    if (id < 0 || id >= kShortcutCount || !IsBindableKey(key)) return RecordOutcome::Ignored;
    const KeyBinding nb{key, ctrl, shift, alt};
    const int other = ShortcutOwner(b, nb, id);
    if (other >= 0) {
        if (owner) *owner = other;
        return RecordOutcome::Conflict;
    }
    b[static_cast<size_t>(id)] = nb;
    return RecordOutcome::Assigned;
}

// Restores action `id` to its default. An action that holds that default now is reset
// too (and so on), so two actions never share a key. Returns every action that changed.
inline std::vector<int> ResetShortcut(ShortcutBindings& b, int id)
{
    std::vector<int> changed;
    if (id < 0 || id >= kShortcutCount) return changed;
    std::vector<int> todo{id};
    for (int guard = 0; !todo.empty() && guard < 4 * kShortcutCount; ++guard) {
        const int k = todo.back();
        todo.pop_back();
        const KeyBinding& d = kShortcutTable[k].def;
        if (b[static_cast<size_t>(k)] != d) {
            b[static_cast<size_t>(k)] = d;
            changed.push_back(k);
        }
        for (int j = 0; j < kShortcutCount; ++j)
            if (j != k && d.vk != 0 && b[static_cast<size_t>(j)] == d &&  // 13-61: no key holds nothing
                ShortcutContextsOverlap(kShortcutTable[j].context, kShortcutTable[k].context))
                todo.push_back(j);
    }
    return changed;
}

// Makes a set of bindings valid (each bindable, no key shared), resetting what is not.
// Returns the actions it reset. Used on load, where the stored values may be stale.
// Story 10-11: `stored` (optional, one flag per action) says which keys the user's settings
// hold. A stored key that clashes with an action still on its default (not stored: a default
// added or changed by a newer RAV) wins: that action goes unbound (vk 0, listed in `unbound`,
// never saved), so it gets its default back by itself on a later load once the key is free.
// Two stored keys (or no `stored`) keep the old rule: the custom one is reset, the later one
// when both are custom. 13-60: a row `linked` to a REAPER action (optional flags) holds no key (vk 0): left as is.
inline std::vector<int> SanitizeShortcuts(ShortcutBindings& b, const bool* stored = nullptr,
                                          std::vector<int>* unbound = nullptr, const bool* linked = nullptr)
{
    std::vector<int> reset;
    auto note = [&reset](const std::vector<int>& v) {
        for (int x : v)
            if (std::find(reset.begin(), reset.end(), x) == reset.end()) reset.push_back(x);
    };
    for (int i = 0; i < kShortcutCount; ++i)
        if (!(linked && linked[i]) && !IsBindableKey(b[static_cast<size_t>(i)].vk)) note(ResetShortcut(b, i));
    auto on_default = [&b](int k) { return b[static_cast<size_t>(k)] == kShortcutTable[k].def; };
    for (int pass = 0; pass < 2 * kShortcutCount; ++pass) {
        bool clash = false;
        for (int i = 0; i < kShortcutCount && !clash; ++i) {
            if (b[static_cast<size_t>(i)].vk == 0) continue;  // unbound: owns no key
            const int j = ShortcutOwner(b, b[static_cast<size_t>(i)], i);
            if (j < 0) continue;
            clash = true;
            if (stored && stored[i] != stored[j]) {
                const int def_side = stored[i] ? j : i;
                if (on_default(def_side)) {
                    b[static_cast<size_t>(def_side)] = KeyBinding{};
                    if (unbound && std::find(unbound->begin(), unbound->end(), def_side) == unbound->end())
                        unbound->push_back(def_side);
                    continue;
                }
            }
            // Reset the custom one (the later one when both are custom).
            const int victim = on_default(j) ? i : std::max(i, j);
            note(ResetShortcut(b, victim));
        }
        if (!clash) break;
    }
    return reset;
}

// 10-11 review: the action that keeps `id` from getting its default back: `id` has no key
// (SanitizeShortcuts left it unbound) and its default is another action's key, the user's own.
// A reset then is refused (ResetShortcut would take the user's key away). -1 = no blocker.
inline int ShortcutResetBlocker(const ShortcutBindings& b, int id)
{
    if (id < 0 || id >= kShortcutCount || b[static_cast<size_t>(id)].vk != 0) return -1;
    return ShortcutOwner(b, kShortcutTable[id].def, id);
}

// 10-11 review: every unbound action whose default key is free again (the action that held it
// took another key or went back to its own default) gets it back at once. Returns them. 13-60: never a
// row `linked` to a REAPER action (optional flags).
inline std::vector<int> RebindFreedDefaults(ShortcutBindings& b, const bool* linked = nullptr)
{
    std::vector<int> back;
    for (int i = 0; i < kShortcutCount; ++i) {
        const KeyBinding& d = kShortcutTable[i].def;
        if ((linked && linked[i]) || b[static_cast<size_t>(i)].vk != 0 || !IsBindableKey(d.vk) || ShortcutOwner(b, d, i) >= 0) continue;
        b[static_cast<size_t>(i)] = d;
        back.push_back(i);
    }
    return back;
}

// ---- ExtState encoding: "Ctrl+Shift+Alt+<decimal vk>", modifiers optional ---------------

inline std::string EncodeShortcut(const KeyBinding& b)
{
    std::string s;
    if (b.ctrl) s += "Ctrl+";
    if (b.shift) s += "Shift+";
    if (b.alt) s += "Alt+";
    s += std::to_string(b.vk);
    return s;
}

// False when the text is not a valid binding (then *out is untouched).
inline bool DecodeShortcut(const char* text, KeyBinding* out)
{
    if (!text || !out) return false;
    KeyBinding b;
    const char* p = text;
    for (;;) {
        if (std::strncmp(p, "Ctrl+", 5) == 0 && !b.ctrl) { b.ctrl = true; p += 5; }
        else if (std::strncmp(p, "Shift+", 6) == 0 && !b.shift) { b.shift = true; p += 6; }
        else if (std::strncmp(p, "Alt+", 4) == 0 && !b.alt) { b.alt = true; p += 4; }
        else break;
    }
    if (*p < '0' || *p > '9') return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(p, &end, 10);
    if (!end || *end != '\0' || v > 0xFE) return false;
    b.vk = static_cast<unsigned>(v);
    if (!IsBindableKey(b.vk)) return false;
    *out = b;
    return true;
}

// ---- persisted settings: the ExtState text of a bool / a float (spec 11-fb-11) -----------

// "1" / "0"; anything else (missing, empty, "true", "10") gives `def`.
inline bool ParsePrefBool(const char* text, bool def)
{
    if (!text) return def;
    if (text[0] == '1' && text[1] == '\0') return true;
    if (text[0] == '0' && text[1] == '\0') return false;
    return def;
}

// A plain decimal number ("123.0", as SavePrefFloat writes it), within +-1e6; anything else
// (missing, empty, trailing text, nan / inf, out of range) gives `def`.
inline float ParsePrefFloat(const char* text, float def)
{
    if (!text || !text[0]) return def;
    char* end = nullptr;
    const double d = std::strtod(text, &end);
    if (!end || end == text || *end != '\0') return def;
    if (!(d == d) || d > 1.0e6 || d < -1.0e6) return def;  // NaN, inf, absurd values
    return static_cast<float>(d);
}

// ---- key names -----------------------------------------------------------------------

// The name of a key: letters, digits, F1-F24, arrows, Space, Enter, Tab, Backspace,
// Delete, Insert, Home, End, PageUp/PageDown, numpad. Any other key asks `fallback`
// (the keyboard layout's own name, GetKeyNameTextW); without one, "Key 0xNN".
inline std::string ShortcutKeyName(unsigned k, std::string (*fallback)(unsigned) = nullptr)
{
    char buf[32];
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) return std::string(1, static_cast<char>(k));
    if (k >= vk::kF1 && k <= vk::kF24) {
        std::snprintf(buf, sizeof(buf), "F%u", k - vk::kF1 + 1);
        return buf;
    }
    if (k >= vk::kNumpad0 && k <= vk::kNumpad0 + 9) {
        std::snprintf(buf, sizeof(buf), "Num %u", k - vk::kNumpad0);
        return buf;
    }
    switch (k) {
    case vk::kLeft:     return "Left";
    case vk::kUp:       return "Up";
    case vk::kRight:    return "Right";
    case vk::kDown:     return "Down";
    case vk::kSpace:    return "Space";
    case vk::kReturn:   return "Enter";
    case vk::kTab:      return "Tab";
    case vk::kBack:     return "Backspace";
    case vk::kDelete:   return "Delete";
    case vk::kInsert:   return "Insert";
    case vk::kHome:     return "Home";
    case vk::kEnd:      return "End";
    case vk::kPrior:    return "PageUp";
    case vk::kNext:     return "PageDown";
    case vk::kMultiply: return "Num *";
    case vk::kAdd:      return "Num +";
    case vk::kSubtract: return "Num -";
    case vk::kDecimal:  return "Num .";
    case vk::kDivide:   return "Num /";
    default: break;
    }
    if (fallback) {
        std::string s = fallback(k);
        if (!s.empty()) return s;
    }
    std::snprintf(buf, sizeof(buf), "Key 0x%02X", k);
    return buf;
}

// "Ctrl+Shift+Alt+K" (modifiers only when held); "" when unbound.
inline std::string FormatShortcut(const KeyBinding& b, std::string (*fallback)(unsigned) = nullptr)
{
    if (b.vk == 0) return "";  // 10-11: unbound (SanitizeShortcuts)
    std::string s;
    if (b.ctrl) s += "Ctrl+";
    if (b.shift) s += "Shift+";
    if (b.alt) s += "Alt+";
    return s + ShortcutKeyName(b.vk, fallback);
}

// 10-11 review: the key in brackets after a text that names it (" (C)"), "" for an action
// without a key (unbound), so no text ever shows an empty "()". Every text that names an
// action's key goes through it (key caps simply show nothing).
inline std::string ShortcutSuffix(const std::string& key, const char* open = " (", const char* close = ")")
{
    return key.empty() ? std::string() : std::string(open) + key + close;
}

// ---- live state (src/shortcuts.cpp, REAPER + Windows) ----------------------------------

// Reads the custom keys from REAPER ExtState (section ReaAnimViewer, key shortcut.<id>).
// A bad stored value falls back to the default with a warning. Call once at load.
void LoadShortcuts();

const ShortcutBindings& CurrentShortcuts();

// The action a key press runs in the viewer (Ctrl / Shift / Alt held as given), or -1.
int ShortcutActionForKey(unsigned key, bool ctrl, bool shift, bool alt, bool video_view, bool tagging_view = false,
                         bool library_edit = false);

// The current key of an action as text ("C", "Shift+V"), for key caps and the popup.
const char* ShortcutKeyLabel(int id);

// The keyboard layout's own name for a key (GetKeyNameTextW), "" when it has none: the fallback of
// ShortcutKeyName / FormatShortcut (13-45: the view rows' keys too).
std::string LayoutKeyName(unsigned key);

// Re-reads the key names from the keyboard layout (after a layout switch).
void RefreshShortcutLabels();

// Right-click in the popup: back to the default (and whatever held it), ExtState cleared.
// 10-11 review: refused for an action without a key whose default is the user's own key for
// another action (ShortcutResetBlocker): returns that action, else -1. Any reset or new key
// that frees an unbound action's default gives it back at once (RebindFreedDefaults).
int ResetShortcutToDefault(int id);

// Recording (double-click in the popup): the next non-modifier key down becomes the
// binding. While it runs, the accelerator hook passes every key to the viewer.
// 13-45: a "View: scroll and zoom" row's recording (view_nav.h) counts too: ShortcutRecordingId()
// is then kShortcutCount + its row, ShortcutRecordKey hands it the key, Cancel ends it.
void BeginShortcutRecording(int id);
void CancelShortcutRecording();  // also on focus loss / parking
int  ShortcutRecordingId();                // -1 when not recording
const char* ShortcutRecordingConflict();   // the label of the action that holds the last key, or nullptr
// A key down while recording (always consumed). Esc cancels.
void ShortcutRecordKey(unsigned key, bool ctrl, bool shift, bool alt);

// ---- lot 13-60: a row linked to a REAPER action ------------------------------------------
// Stored in the row's ExtState entry as "link:<id>" (action_link.h); the action's keys are read from
// reaper-kb.ini, again whenever REAPER saves it (view_nav.h ReaperKbGeneration). A character key REAPER
// binds ("+") is the key that types it on this keyboard layout. Recording a key, or right-click (back to
// the default), ends the link.
const ShortcutKeymap& CurrentShortcutKeymap();
struct KbGesture;  // reaper_view_keys.h
// The key a REAPER key gesture is on this keyboard: a character binding ("+") is the key typing it on this
// layout (VkKeyScanEx: its Shift / Ctrl / Alt added). False for the wheel, or a character no key types.
bool KeyBindingOfGesture(const KbGesture& g, KeyBinding* out);
// The row's action id ("" = its own key).
const std::string& ShortcutLink(int id);
void LinkShortcutToAction(int id, const std::string& cmd);
// ---- lot 13-60b: every row REAPER | Own ---------------------------------------------------------
// REAPER: the row runs on the keys REAPER gives its own action (ShortcutReaperActionId), set in REAPER's
// Actions window, followed live; it holds no key of its own then (like a linked row). Own: its own key,
// or a REAPER action the user picked (the chain). Stored in the same ExtState entry ("reaper").
bool ShortcutFollowsReaper(int id);
// 13-63: the row is on REAPER and runs on its fallback key (ShortcutReaperFallback): REAPER lists no key
// for its action now.
bool ShortcutOnReaperFallback(int id);
void SetShortcutFollowsReaper(int id);
// Back to Own from REAPER: the row's default key (ResetShortcutToDefault: the action whose own key holds
// it, else -1). 13-61 / 13-63: a REAPER-default row: its fallback key when free, else no key (-1).
int SetShortcutOwn(int id);
// The action whose keys run the row ("" = its own key): the picked one, or its own on REAPER.
std::string ShortcutFollowedAction(int id);
// "Ctrl+K" of a key (the layout's names).
std::string ShortcutBindingLabel(const KeyBinding& b);

// ---- small persisted settings (spec 11-fb-11) ------------------------------------------
// One persistence path for the viewer's own settings: REAPER ExtState, the same section as
// the keys (ReaAnimViewer), kept across sessions. A missing or unreadable value gives `def`.
bool  LoadPrefBool(const char* key, bool def);
void  SavePrefBool(const char* key, bool value);
float LoadPrefFloat(const char* key, float def);
void  SavePrefFloat(const char* key, float value);

}  // namespace rav
