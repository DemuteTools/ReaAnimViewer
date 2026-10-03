// SPDX-License-Identifier: MIT
//
// Host test of the viewer's shortcut rules (src/shortcuts.h, spec 11-fb-3): matching with
// exact modifiers, recording (rebind, conflict, cancel, modifier only), reset and its
// collision rule, the ExtState encoding and the key names. No REAPER, no Windows.
//   c++ -std=c++17 -I src tests/shortcuts_test.cpp -o t && ./t

#include "shortcuts.h"

#include <cstdio>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

KeyBinding Key(unsigned k, bool ctrl = false, bool shift = false, bool alt = false)
{
    return KeyBinding{k, ctrl, shift, alt};
}

bool Contains(const std::vector<int>& v, int x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}

}  // namespace

int main()
{
    // Defaults: V, P, C; C only in Video view.
    {
        const ShortcutBindings b = DefaultShortcutBindings();
        CHECK(b[kShortcutToggleView] == Key('V'));
        CHECK(b[kShortcutTogglePanel] == Key('P'));
        CHECK(b[kShortcutCut] == Key('C'));
        CHECK(ShortcutActionFor(b, 'V', false, false, false, false) == kShortcutToggleView);
        CHECK(ShortcutActionFor(b, 'P', false, false, false, false) == kShortcutTogglePanel);
        CHECK(ShortcutActionFor(b, 'C', false, false, false, true) == kShortcutCut);
        CHECK(ShortcutActionFor(b, 'C', false, false, false, false) == -1);  // RAV view: C stays REAPER's
        CHECK(ShortcutActionFor(b, vk::kSpace, false, false, false, true) == -1);  // Space stays REAPER's
        // Exact modifiers: Ctrl+V / Shift+V are not V.
        CHECK(ShortcutActionFor(b, 'V', true, false, false, false) == -1);
        CHECK(ShortcutActionFor(b, 'V', false, true, false, false) == -1);
        CHECK(ShortcutActionFor(b, 'V', false, false, true, false) == -1);
    }

    // Spec 11-fb-11: Delete deletes the current shot, in Video view only.
    {
        const ShortcutBindings b = DefaultShortcutBindings();
        CHECK(b[kShortcutDeleteShot] == Key(vk::kDelete));
        CHECK(std::string(kShortcutTable[kShortcutDeleteShot].id) == "delete_shot");
        CHECK(std::string(kShortcutTable[kShortcutDeleteShot].label) == "Delete current shot");
        CHECK(kShortcutTable[kShortcutDeleteShot].context == ShortcutContext::VideoView);
        CHECK(IsBindableKey(vk::kDelete));
        CHECK(ShortcutActionFor(b, vk::kDelete, false, false, false, true) == kShortcutDeleteShot);
        CHECK(ShortcutActionFor(b, vk::kDelete, false, false, false, false) == -1);  // RAV view: REAPER's
        CHECK(ShortcutActionFor(b, vk::kDelete, true, false, false, true) == -1);    // exact modifiers
        CHECK(FormatShortcut(b[kShortcutDeleteShot]) == "Delete");
        CHECK(EncodeShortcut(b[kShortcutDeleteShot]) == "46");
        KeyBinding k;
        CHECK(DecodeShortcut("46", &k) && k == Key(vk::kDelete));
        // Routing: the key reaches the viewer in Video view only; a text field takes it anyway.
        KeyRouteState st;
        KeyRouteInput del;
        del.msg = KeyMsg::KeyDown;
        del.key = vk::kDelete;
        CHECK(RouteViewerKey(b, del, st) == kRouteReaper);
        del.video_view = true;
        CHECK(RouteViewerKey(b, del, st) == kRouteViewer);
        KeyRouteInput up;
        up.msg = KeyMsg::KeyUp;
        up.key = vk::kDelete;
        CHECK(RouteViewerKey(b, up, st) == kRouteViewer);
        CHECK(st.claimed_vk == 0);
        // Esc while the delete confirmation is open (popup_open): the viewer's.
        KeyRouteInput esc;
        esc.msg = KeyMsg::KeyDown;
        esc.key = vk::kEscape;
        esc.video_view = true;
        CHECK(RouteViewerKey(b, esc, st) == kRouteReaper);
        esc.popup_open = true;
        CHECK(RouteViewerKey(b, esc, st) == kRouteViewer);
        // Rebind: Delete can go to another key, and Cut cannot take Delete.
        ShortcutBindings r = DefaultShortcutBindings();
        int owner = -1;
        CHECK(ShortcutRecordStep(r, kShortcutCut, vk::kDelete, false, false, false, &owner) == RecordOutcome::Conflict);
        CHECK(owner == kShortcutDeleteShot);
        CHECK(ShortcutRecordStep(r, kShortcutDeleteShot, vk::kBack, false, false, false) == RecordOutcome::Assigned);
        CHECK(ShortcutActionFor(r, vk::kBack, false, false, false, true) == kShortcutDeleteShot);
        CHECK(ShortcutActionFor(r, vk::kDelete, false, false, false, true) == -1);
        CHECK(ResetShortcut(r, kShortcutDeleteShot).size() == 1);
        CHECK(r == DefaultShortcutBindings());
    }

    // Spec 11-fb-11: the persisted settings' text.
    {
        CHECK(ParsePrefBool("1", false) == true);
        CHECK(ParsePrefBool("0", true) == false);
        CHECK(ParsePrefBool("", true) == true);
        CHECK(ParsePrefBool(nullptr, false) == false);
        CHECK(ParsePrefBool("10", false) == false);
        CHECK(ParsePrefBool("true", false) == false);
        CHECK(ParsePrefBool("true", true) == true);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", 123.0);  // SavePrefFloat's format
        CHECK(ParsePrefFloat(buf, 64.0f) == 123.0f);
        CHECK(ParsePrefFloat("123.0", 64.0f) == 123.0f);
        CHECK(ParsePrefFloat("", 64.0f) == 64.0f);
        CHECK(ParsePrefFloat(nullptr, 64.0f) == 64.0f);
        CHECK(ParsePrefFloat("abc", 64.0f) == 64.0f);
        CHECK(ParsePrefFloat("12x", 64.0f) == 64.0f);
        CHECK(ParsePrefFloat("nan", 64.0f) == 64.0f);
        CHECK(ParsePrefFloat("inf", 64.0f) == 64.0f);
        CHECK(ParsePrefFloat("1e9", 64.0f) == 64.0f);
    }

    // Enter: the viewer's only while the delete confirmation is open.
    {
        const ShortcutBindings b = DefaultShortcutBindings();
        KeyRouteState st;
        KeyRouteInput enter;
        enter.msg = KeyMsg::KeyDown;
        enter.key = vk::kReturn;
        enter.video_view = true;
        CHECK(RouteViewerKey(b, enter, st) == kRouteReaper);
        enter.popup_open = true;  // the Shortcuts popup alone does not take Enter
        CHECK(RouteViewerKey(b, enter, st) == kRouteReaper);
        enter.confirm_open = true;
        CHECK(RouteViewerKey(b, enter, st) == kRouteViewer);
        KeyRouteInput up;
        up.msg = KeyMsg::KeyUp;
        up.key = vk::kReturn;
        CHECK(RouteViewerKey(b, up, st) == kRouteViewer);
        CHECK(st.claimed_vk == 0);
    }

    // Matching rule.
    CHECK(ShortcutMatches(Key('K'), 'K', false, false, false));
    CHECK(!ShortcutMatches(Key('K'), 'J', false, false, false));
    CHECK(ShortcutMatches(Key('K', true, true, true), 'K', true, true, true));
    CHECK(!ShortcutMatches(Key('K', true, false, false), 'K', true, true, false));
    CHECK(!ShortcutMatches(Key(0), 0, false, false, false));  // no binding never matches

    // Rebind: double-click Cut, press K -> Cut = K, C no longer ours.
    {
        ShortcutBindings b = DefaultShortcutBindings();
        int owner = 7;
        CHECK(ShortcutRecordStep(b, kShortcutCut, 'K', false, false, false, &owner) == RecordOutcome::Assigned);
        CHECK(owner == -1);
        CHECK(b[kShortcutCut] == Key('K'));
        CHECK(ShortcutActionFor(b, 'K', false, false, false, true) == kShortcutCut);
        CHECK(ShortcutActionFor(b, 'C', false, false, false, true) == -1);
        CHECK(EncodeShortcut(b[kShortcutCut]) == "75");
    }

    // With modifier: Shift+V for the view toggle -> only Shift+V toggles, plain V is REAPER's.
    {
        ShortcutBindings b = DefaultShortcutBindings();
        CHECK(ShortcutRecordStep(b, kShortcutToggleView, 'V', false, true, false) == RecordOutcome::Assigned);
        CHECK(b[kShortcutToggleView] == Key('V', false, true, false));
        CHECK(ShortcutActionFor(b, 'V', false, true, false, false) == kShortcutToggleView);
        CHECK(ShortcutActionFor(b, 'V', false, false, false, false) == -1);
    }

    // Conflict: P for Cut -> refused, owner = the panel toggle, still recording (unchanged).
    {
        ShortcutBindings b = DefaultShortcutBindings();
        int owner = -1;
        CHECK(ShortcutRecordStep(b, kShortcutCut, 'P', false, false, false, &owner) == RecordOutcome::Conflict);
        CHECK(owner == kShortcutTogglePanel);
        CHECK(std::string(kShortcutTable[owner].label) == "Show / hide the Video panel");
        CHECK(b[kShortcutCut] == Key('C'));
        // Its own key again is fine (no change).
        CHECK(ShortcutRecordStep(b, kShortcutCut, 'C', false, false, false) == RecordOutcome::Assigned);
        CHECK(b == DefaultShortcutBindings());
        // Shift+P is not P: no conflict.
        CHECK(ShortcutRecordStep(b, kShortcutCut, 'P', false, true, false) == RecordOutcome::Assigned);
    }

    // Cancel: Esc -> unchanged (even with a modifier held).
    {
        ShortcutBindings b = DefaultShortcutBindings();
        CHECK(ShortcutRecordStep(b, kShortcutCut, vk::kEscape, false, false, false) == RecordOutcome::Cancelled);
        CHECK(ShortcutRecordStep(b, kShortcutCut, vk::kEscape, false, true, false) == RecordOutcome::Cancelled);
        CHECK(b == DefaultShortcutBindings());
    }

    // Modifier only: Ctrl alone (and the other modifiers / locks) -> still waiting.
    {
        ShortcutBindings b = DefaultShortcutBindings();
        const unsigned mods[] = {vk::kControl, vk::kShift, vk::kMenu, 0xA2u, 0xA1u, 0xA5u, vk::kLWin, vk::kCapital};
        for (unsigned m : mods)
            CHECK(ShortcutRecordStep(b, kShortcutCut, m, m == vk::kControl, false, false) == RecordOutcome::Ignored);
        CHECK(ShortcutRecordStep(b, kShortcutCut, 0x01, false, false, false) == RecordOutcome::Ignored);  // mouse
        CHECK(b == DefaultShortcutBindings());
    }

    // Reset: right-click Cut -> C again.
    {
        ShortcutBindings b = DefaultShortcutBindings();
        b[kShortcutCut] = Key('K');
        const std::vector<int> changed = ResetShortcut(b, kShortcutCut);
        CHECK(b[kShortcutCut] == Key('C'));
        CHECK(changed.size() == 1 && changed[0] == kShortcutCut);
        CHECK(ResetShortcut(b, kShortcutCut).empty());  // already default
    }

    // Reset collision: View = C (Cut was K), reset Cut -> Cut = C and View back to V.
    {
        ShortcutBindings b = DefaultShortcutBindings();
        CHECK(ShortcutRecordStep(b, kShortcutCut, 'K', false, false, false) == RecordOutcome::Assigned);
        CHECK(ShortcutRecordStep(b, kShortcutToggleView, 'C', false, false, false) == RecordOutcome::Assigned);
        const std::vector<int> changed = ResetShortcut(b, kShortcutCut);
        CHECK(b[kShortcutCut] == Key('C'));
        CHECK(b[kShortcutToggleView] == Key('V'));
        CHECK(Contains(changed, kShortcutCut) && Contains(changed, kShortcutToggleView));
        CHECK(b == DefaultShortcutBindings());
    }
    // ...and a chain: Panel = V, View = C, Cut = K; reset Cut -> all three defaults.
    {
        ShortcutBindings b = DefaultShortcutBindings();
        b[kShortcutCut] = Key('K');
        b[kShortcutToggleView] = Key('C');
        b[kShortcutTogglePanel] = Key('V');
        const std::vector<int> changed = ResetShortcut(b, kShortcutCut);
        CHECK(b == DefaultShortcutBindings());
        CHECK(changed.size() == 3);
    }

    // ExtState encoding round trip; bad stored values are refused.
    {
        KeyBinding k;
        CHECK(DecodeShortcut("75", &k) && k == Key('K'));
        CHECK(DecodeShortcut(EncodeShortcut(Key('V', true, true, true)).c_str(), &k) && k == Key('V', true, true, true));
        CHECK(EncodeShortcut(Key('V', true, false, true)) == "Ctrl+Alt+86");
        CHECK(DecodeShortcut("Alt+Shift+86", &k) && k == Key('V', false, true, true));
        k = Key('Z');
        CHECK(!DecodeShortcut("", &k));
        CHECK(!DecodeShortcut("garbage", &k));
        CHECK(!DecodeShortcut("Ctrl+", &k));
        CHECK(!DecodeShortcut("75x", &k));
        CHECK(!DecodeShortcut("-75", &k));
        CHECK(!DecodeShortcut("999", &k));
        CHECK(!DecodeShortcut("27", &k));   // Esc
        CHECK(!DecodeShortcut("17", &k));   // Ctrl alone
        CHECK(!DecodeShortcut("1", &k));    // left mouse button
        CHECK(!DecodeShortcut("Ctrl+Ctrl+75", &k));
        CHECK(!DecodeShortcut("255", &k));       // 0xFF: no key
        CHECK(!DecodeShortcut("Ctrl+27", &k));   // Esc, even with a modifier
        CHECK(!DecodeShortcut("229", &k));       // VK_PROCESSKEY (IME)
        CHECK(!DecodeShortcut("231", &k));       // VK_PACKET
        CHECK(k == Key('Z'));  // untouched on failure
        CHECK(DecodeShortcut("254", &k) && k == Key(0xFE));  // the last bindable key
        CHECK(DecodeShortcut("Shift+Ctrl+75", &k) && k == Key('K', true, true, false));  // any modifier order
    }

    // Load: stale stored values that clash are made valid (custom one reset).
    {
        ShortcutBindings b = DefaultShortcutBindings();
        b[kShortcutToggleView] = Key('C');  // clashes with Cut's default
        const std::vector<int> reset = SanitizeShortcuts(b);
        CHECK(b == DefaultShortcutBindings());
        CHECK(reset.size() == 1 && reset[0] == kShortcutToggleView);
        b[kShortcutCut] = Key('K');
        b[kShortcutTogglePanel] = Key('K');  // both custom: the later one goes
        const std::vector<int> both = SanitizeShortcuts(b);
        CHECK(b[kShortcutTogglePanel] == Key('K'));
        CHECK(b[kShortcutCut] == Key('C'));
        CHECK(both.size() == 1 && both[0] == kShortcutCut);
        b = DefaultShortcutBindings();
        CHECK(SanitizeShortcuts(b).empty());
        // An unbindable stored key (none / Esc / a modifier) goes back to its default.
        b[kShortcutToggleView] = Key(0);
        b[kShortcutTogglePanel] = Key(vk::kEscape);
        b[kShortcutCut] = Key(vk::kControl, true);
        const std::vector<int> bad = SanitizeShortcuts(b);
        CHECK(b == DefaultShortcutBindings());
        CHECK(bad.size() == 3);
    }

    // Unbindable keys are never recorded (IME process key, injected Unicode, 0xFF).
    {
        ShortcutBindings b = DefaultShortcutBindings();
        CHECK(ShortcutRecordStep(b, kShortcutCut, vk::kProcessKey, false, false, false) == RecordOutcome::Ignored);
        CHECK(ShortcutRecordStep(b, kShortcutCut, vk::kPacket, false, false, false) == RecordOutcome::Ignored);
        CHECK(ShortcutRecordStep(b, kShortcutCut, 0xFF, false, false, false) == RecordOutcome::Ignored);
        CHECK(b == DefaultShortcutBindings());
    }

    // Key routing (the accelerator hook's decision).
    {
        const ShortcutBindings b = DefaultShortcutBindings();
        auto msg = [](KeyMsg m, unsigned key) {
            KeyRouteInput in;
            in.msg = m;
            in.key = key;
            return in;
        };
        // A bound key: its down, its character and its release are the viewer's.
        KeyRouteState st;
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyDown, 'V'), st) == kRouteViewer);
        CHECK(st.claimed_vk == 'V');
        CHECK(RouteViewerKey(b, msg(KeyMsg::Char, 'v'), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyUp, 'V'), st) == kRouteViewer);
        CHECK(st.claimed_vk == 0 && st.released_vk == 0);
        // Space stays REAPER's (down, character, release).
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyDown, vk::kSpace), st) == kRouteReaper);
        CHECK(RouteViewerKey(b, msg(KeyMsg::Char, ' '), st) == kRouteReaper);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyUp, vk::kSpace), st) == kRouteReaper);
        // An unbound character after a claim was released: REAPER's.
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyDown, 'P'), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyUp, 'P'), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, msg(KeyMsg::Char, 'x'), st) == kRouteReaper);
        // ...and after an unbound key down (a lost release): the claim goes.
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyDown, 'V'), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyDown, 'X'), st) == kRouteReaper);
        CHECK(st.claimed_vk == 0);
        CHECK(RouteViewerKey(b, msg(KeyMsg::Char, 'x'), st) == kRouteReaper);
        // The cut key in RAV view stays REAPER's.
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyDown, 'C'), st) == kRouteReaper);
        KeyRouteInput cut = msg(KeyMsg::KeyDown, 'C');
        cut.video_view = true;
        CHECK(RouteViewerKey(b, cut, st) == kRouteViewer);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyUp, 'C'), st) == kRouteViewer);
        // Esc: the viewer's only while the popup is open.
        st = KeyRouteState{};
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyDown, vk::kEscape), st) == kRouteReaper);
        KeyRouteInput esc = msg(KeyMsg::KeyDown, vk::kEscape);
        esc.popup_open = true;
        CHECK(RouteViewerKey(b, esc, st) == kRouteViewer);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyUp, vk::kEscape), st) == kRouteViewer);
        // An Alt binding: WM_SYSKEYDOWN / WM_SYSCHAR / WM_SYSKEYUP handed over with -20, and
        // the release is marked for the window to swallow.
        ShortcutBindings alt = DefaultShortcutBindings();
        alt[kShortcutToggleView] = Key('V', false, false, true);
        st = KeyRouteState{};
        KeyRouteInput av = msg(KeyMsg::SysKeyDown, 'V');
        av.alt = true;
        CHECK(RouteViewerKey(alt, av, st) == kRouteViewerSys);
        CHECK(RouteViewerKey(alt, msg(KeyMsg::SysChar, 'v'), st) == kRouteViewerSys);
        CHECK(RouteViewerKey(alt, msg(KeyMsg::SysKeyUp, 'V'), st) == kRouteViewerSys);
        CHECK(st.claimed_vk == 0 && st.released_vk == 'V');
        // Plain V is now REAPER's; Alt alone too.
        CHECK(RouteViewerKey(alt, msg(KeyMsg::KeyDown, 'V'), st) == kRouteReaper);
        CHECK(st.released_vk == 0);
        CHECK(RouteViewerKey(alt, msg(KeyMsg::SysKeyDown, vk::kMenu), st) == kRouteReaper);
        // Auto-repeat of the claimed key is claimed even when it maps to nothing now (the
        // key just recorded for Cut, a Video view action, held in RAV view).
        ShortcutBindings rec = DefaultShortcutBindings();
        st = KeyRouteState{};
        KeyRouteInput rk = msg(KeyMsg::KeyDown, 'K');
        rk.recording = true;
        CHECK(RouteViewerKey(rec, rk, st) == kRouteViewer);
        CHECK(st.claimed_vk == 'K');
        CHECK(ShortcutRecordStep(rec, kShortcutCut, 'K', false, false, false) == RecordOutcome::Assigned);
        KeyRouteInput rep = msg(KeyMsg::KeyDown, 'K');
        rep.repeat = true;
        CHECK(RouteViewerKey(rec, rep, st) == kRouteViewer);
        CHECK(RouteViewerKey(rec, msg(KeyMsg::Char, 'k'), st) == kRouteViewer);
        CHECK(RouteViewerKey(rec, msg(KeyMsg::KeyUp, 'K'), st) == kRouteViewer);
        CHECK(st.claimed_vk == 0);
        CHECK(RouteViewerKey(rec, rep, st) == kRouteReaper);  // no claim: a repeat is REAPER's
        // Recording: every key, Alt ones with -20.
        KeyRouteInput ra = msg(KeyMsg::SysKeyDown, 'X');
        ra.recording = true;
        CHECK(RouteViewerKey(rec, ra, st) == kRouteViewerSys);
        // A text field: every key.
        KeyRouteInput tf = msg(KeyMsg::KeyDown, vk::kSpace);
        tf.text_input = true;
        CHECK(RouteViewerKey(rec, tf, st) == kRouteViewer);
    }

    // Key names.
    CHECK(FormatShortcut(Key('C')) == "C");
    CHECK(FormatShortcut(Key('7')) == "7");
    CHECK(FormatShortcut(Key('V', true, true, true)) == "Ctrl+Shift+Alt+V");
    CHECK(FormatShortcut(Key(vk::kF1)) == "F1");
    CHECK(FormatShortcut(Key(vk::kF24)) == "F24");
    CHECK(FormatShortcut(Key(vk::kSpace, false, true)) == "Shift+Space");
    CHECK(FormatShortcut(Key(vk::kPrior)) == "PageUp");
    CHECK(FormatShortcut(Key(vk::kNext)) == "PageDown");
    CHECK(FormatShortcut(Key(vk::kLeft)) == "Left");
    CHECK(FormatShortcut(Key(vk::kNumpad0 + 5)) == "Num 5");
    CHECK(FormatShortcut(Key(vk::kAdd)) == "Num +");
    CHECK(FormatShortcut(Key(vk::kReturn)) == "Enter");
    CHECK(FormatShortcut(Key(vk::kBack)) == "Backspace");
    CHECK(FormatShortcut(Key(0xBA)) == "Key 0xBA");  // no layout here: the fallback text
    CHECK(FormatShortcut(Key(0xBA), [](unsigned) { return std::string(";"); }) == ";");

    if (g_fails == 0) std::printf("shortcuts: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
