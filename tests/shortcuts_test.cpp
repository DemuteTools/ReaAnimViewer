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

    // Ctrl+C / Ctrl+V copy and paste the current shot's camera, in Video view only.
    {
        const ShortcutBindings b = DefaultShortcutBindings();
        CHECK(b[kShortcutCopyCamera] == Key('C', true));
        CHECK(b[kShortcutPasteCamera] == Key('V', true));
        CHECK(ShortcutActionFor(b, 'C', true, false, false, true) == kShortcutCopyCamera);
        CHECK(ShortcutActionFor(b, 'V', true, false, false, true) == kShortcutPasteCamera);
        CHECK(ShortcutActionFor(b, 'C', true, false, false, false) == -1);  // RAV view: REAPER's
        CHECK(ShortcutActionFor(b, 'V', true, false, false, false) == -1);
        CHECK(FormatShortcut(b[kShortcutCopyCamera]) == "Ctrl+C");
        ShortcutBindings d = b;
        CHECK(SanitizeShortcuts(d).empty());  // no default clash
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

    // Without the stored flags (10-11: the load passes them, see below): stale values that clash
    // are made valid (custom one reset).
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

    // 10-11: on load a stored key beats an action still on its (new) default: that action goes
    // unbound, nothing reset, and it gets its default back once the key is free.
    {
        ShortcutBindings b = DefaultShortcutBindings();
        bool stored[kShortcutCount] = {};
        b[kShortcutToggleView] = Key('C');  // the user's key; Cut's default is C (a "new" default)
        stored[kShortcutToggleView] = true;
        std::vector<int> unbound;
        const std::vector<int> reset = SanitizeShortcuts(b, stored, &unbound);
        CHECK(reset.empty());
        CHECK(b[kShortcutToggleView] == Key('C'));
        CHECK(b[kShortcutCut].vk == 0);
        CHECK(unbound.size() == 1 && unbound[0] == kShortcutCut);
        CHECK(FormatShortcut(b[kShortcutCut]).empty());
        // Unbound matches no key.
        CHECK(ShortcutActionFor(b, 'C', false, false, false, true) == kShortcutToggleView);
        CHECK(ShortcutActionFor(b, 0, false, false, false, true) == -1);
        // A later load with the key free: the default comes back (nothing was saved for it).
        ShortcutBindings later = DefaultShortcutBindings();
        bool stored2[kShortcutCount] = {};
        later[kShortcutToggleView] = Key('K');
        stored2[kShortcutToggleView] = true;
        std::vector<int> none;
        CHECK(SanitizeShortcuts(later, stored2, &none).empty() && none.empty());
        CHECK(later[kShortcutCut] == Key('C'));
        // 10-11 review: restoring the unbound action is refused while its default is your key
        // (Cut cannot take C back from Next view); a bound action never has a blocker.
        CHECK(ShortcutResetBlocker(b, kShortcutCut) == kShortcutToggleView);
        CHECK(ShortcutResetBlocker(b, kShortcutToggleView) == -1);
        CHECK(RebindFreedDefaults(b).empty() && b[kShortcutCut].vk == 0);  // C still yours
        // Next view takes another key: Cut gets C back at once (no reload), and is no longer blocked.
        b[kShortcutToggleView] = Key('K');
        const std::vector<int> back = RebindFreedDefaults(b);
        CHECK(back.size() == 1 && back[0] == kShortcutCut && b[kShortcutCut] == Key('C'));
        CHECK(ShortcutResetBlocker(b, kShortcutCut) == -1);
        // ... and the same when Next view goes back to its own default (ResetShortcut, then rebind).
        b[kShortcutToggleView] = Key('C');
        b[kShortcutCut] = KeyBinding{};
        ResetShortcut(b, kShortcutToggleView);
        CHECK(RebindFreedDefaults(b).size() == 1 && b == DefaultShortcutBindings());
        // An unbound default whose key another context holds stays free to come back: Delete
        // in Tagging view while Video view's Delete is bound (contexts never on together).
        ShortcutBindings ctx = DefaultShortcutBindings();
        ctx[kShortcutTagDelEvent] = KeyBinding{};
        CHECK(RebindFreedDefaults(ctx).size() == 1 && ctx == DefaultShortcutBindings());
        // The suffix that names a key in a text: nothing for an action without a key.
        CHECK(ShortcutSuffix("").empty() && ShortcutSuffix("E") == " (E)" && ShortcutSuffix("E", "  (") == "  (E)");
        // Two stored keys that clash: the old rule (the later custom one is reset).
        ShortcutBindings two = DefaultShortcutBindings();
        bool stored3[kShortcutCount] = {};
        two[kShortcutCut] = Key('K');
        two[kShortcutTogglePanel] = Key('K');
        stored3[kShortcutCut] = stored3[kShortcutTogglePanel] = true;
        std::vector<int> u3;
        const std::vector<int> r3 = SanitizeShortcuts(two, stored3, &u3);
        CHECK(u3.empty() && r3.size() == 1 && r3[0] == kShortcutCut);
        CHECK(two[kShortcutTogglePanel] == Key('K') && two[kShortcutCut] == Key('C'));
        // A stored key that holds another action's default and is not its own default: when the
        // other side is stored and custom too, the old rule applies; when it is not stored and on
        // its default, it goes unbound even if it comes first in the table.
        ShortcutBindings first = DefaultShortcutBindings();
        bool stored4[kShortcutCount] = {};
        const KeyBinding view_def = first[kShortcutToggleView];
        first[kShortcutCut] = view_def;
        stored4[kShortcutCut] = true;
        std::vector<int> u4;
        CHECK(SanitizeShortcuts(first, stored4, &u4).empty());
        CHECK(first[kShortcutCut] == view_def && first[kShortcutToggleView].vk == 0);
        CHECK(u4.size() == 1 && u4[0] == kShortcutToggleView);
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
        // 13-7: Enter / Esc that close a text field are released after it closed: the release
        // (and an auto-repeat) still reaches the viewer, else ImGui keeps the key down and the
        // next field validates / cancels itself the moment it opens.
        st = KeyRouteState{};
        KeyRouteInput enter = msg(KeyMsg::KeyDown, vk::kReturn);
        enter.text_input = true;
        CHECK(RouteViewerKey(b, enter, st) == kRouteViewer);
        KeyRouteInput enter_rep = msg(KeyMsg::KeyDown, vk::kReturn);
        enter_rep.repeat = true;
        CHECK(RouteViewerKey(b, enter_rep, st) == kRouteViewer);           // field closed, key held
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyUp, vk::kReturn), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyUp, vk::kReturn), st) == kRouteReaper);  // once only
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyDown, vk::kReturn), st) == kRouteReaper);
        KeyRouteInput esc_tf = msg(KeyMsg::KeyDown, vk::kEscape);
        esc_tf.text_input = true;
        CHECK(RouteViewerKey(b, esc_tf, st) == kRouteViewer);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyUp, vk::kEscape), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyUp, vk::kEscape), st) == kRouteReaper);
        // A key released while the field is still active: nothing left over.
        KeyRouteInput a_down = msg(KeyMsg::KeyDown, 'A');
        a_down.text_input = true;
        KeyRouteInput a_up = msg(KeyMsg::KeyUp, 'A');
        a_up.text_input = true;
        CHECK(RouteViewerKey(b, a_down, st) == kRouteViewer);
        CHECK(RouteViewerKey(b, a_up, st) == kRouteViewer);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyUp, 'A'), st) == kRouteReaper);
        // A lost release (focus moved): the next plain press forgets the mark.
        CHECK(RouteViewerKey(b, a_down, st) == kRouteViewer);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyDown, 'A'), st) == kRouteReaper);
        CHECK(RouteViewerKey(b, msg(KeyMsg::KeyUp, 'A'), st) == kRouteReaper);
        // An Alt key typed in a field and released after: -20 and swallowed (no menu bar).
        KeyRouteInput alt_tf = msg(KeyMsg::SysKeyDown, vk::kMenu);
        alt_tf.text_input = true;
        CHECK(RouteViewerKey(b, alt_tf, st) == kRouteViewerSys);
        CHECK(RouteViewerKey(b, msg(KeyMsg::SysKeyUp, vk::kMenu), st) == kRouteViewerSys);
        CHECK(st.released_vk == vk::kMenu);
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

    // Spec 11-fb-12: Alt + wheel over the viewer's UI (the strip's zoom) and Alt's release after it.
    {
        const ShortcutBindings b = DefaultShortcutBindings();
        KeyRouteState st;
        KeyRouteInput wheel;
        wheel.msg = KeyMsg::Wheel;
        wheel.alt = true;
        wheel.imgui_mouse = true;
        KeyRouteInput alt_down;
        alt_down.msg = KeyMsg::SysKeyDown;
        alt_down.key = vk::kMenu;
        alt_down.alt = true;
        KeyRouteInput alt_up;
        alt_up.msg = KeyMsg::SysKeyUp;
        alt_up.key = vk::kMenu;
        // Plain Alt press / release without a wheel: REAPER's.
        CHECK(RouteViewerKey(b, alt_down, st) == kRouteReaper);
        CHECK(RouteViewerKey(b, alt_up, st) == kRouteReaper);
        CHECK(st.released_vk == 0);
        // Alt + wheel over the UI: the viewer's; the following Alt release too, swallowed.
        CHECK(RouteViewerKey(b, alt_down, st) == kRouteReaper);
        CHECK(RouteViewerKey(b, wheel, st) == kRouteViewer);
        CHECK(st.alt_wheel_taken);
        CHECK(RouteViewerKey(b, alt_up, st) == kRouteViewerSys);
        CHECK(st.released_vk == vk::kMenu);
        CHECK(!st.alt_wheel_taken);
        // 13-45: a wheel without Alt over the UI is the viewer's too (nothing taken: no Alt release
        // to swallow); not over the UI: REAPER's.
        KeyRouteState st2;
        KeyRouteInput plain = wheel;
        plain.alt = false;
        CHECK(RouteViewerKey(b, plain, st2) == kRouteViewer);
        // 13-45 review: a Ctrl+wheel over the UI is the viewer's only when a timeline under the mouse
        // runs it (view_wheel); otherwise REAPER's, as before. The horizontal wheel without Ctrl is the
        // viewer's anywhere (over the 3D view too: REAPER's arrange must not scroll behind).
        KeyRouteInput ctrl_wheel = plain;
        ctrl_wheel.ctrl = true;
        CHECK(RouteViewerKey(b, ctrl_wheel, st2) == kRouteReaper);
        ctrl_wheel.view_wheel = true;
        CHECK(RouteViewerKey(b, ctrl_wheel, st2) == kRouteViewer);
        KeyRouteInput ctrl_shift = ctrl_wheel;
        ctrl_shift.shift = true;
        ctrl_shift.view_wheel = false;
        CHECK(RouteViewerKey(b, ctrl_shift, st2) == kRouteReaper);
        KeyRouteInput bound_off_ui = ctrl_wheel;  // the hook never sets view_wheel off the UI; still the viewer's
        bound_off_ui.imgui_mouse = false;
        CHECK(RouteViewerKey(b, bound_off_ui, st2) == kRouteViewer);
        KeyRouteInput hwheel = plain;
        hwheel.horizontal = true;
        CHECK(RouteViewerKey(b, hwheel, st2) == kRouteViewer);
        hwheel.imgui_mouse = false;  // over the 3D view
        CHECK(RouteViewerKey(b, hwheel, st2) == kRouteViewer);
        hwheel.ctrl = true;
        CHECK(RouteViewerKey(b, hwheel, st2) == kRouteReaper);
        KeyRouteInput plain_off = plain;
        plain_off.imgui_mouse = false;  // the 3D view's own wheel: as before
        CHECK(RouteViewerKey(b, plain_off, st2) == kRouteReaper);
        CHECK(!st2.alt_wheel_taken);
        KeyRouteInput off_ui = wheel;
        off_ui.imgui_mouse = false;
        CHECK(RouteViewerKey(b, off_ui, st2) == kRouteReaper);
        CHECK(!st2.alt_wheel_taken);
        // A fresh Alt press clears the state: its plain release is REAPER's.
        KeyRouteState st3;
        CHECK(RouteViewerKey(b, wheel, st3) == kRouteViewer);
        CHECK(RouteViewerKey(b, alt_down, st3) == kRouteReaper);
        CHECK(!st3.alt_wheel_taken);
        CHECK(RouteViewerKey(b, alt_up, st3) == kRouteReaper);
        // An auto-repeat of Alt keeps it.
        KeyRouteState st4;
        CHECK(RouteViewerKey(b, wheel, st4) == kRouteViewer);
        KeyRouteInput alt_rep = alt_down;
        alt_rep.repeat = true;
        RouteViewerKey(b, alt_rep, st4);
        CHECK(st4.alt_wheel_taken);
        // While recording: the release goes through the recording path.
        KeyRouteState st5;
        CHECK(RouteViewerKey(b, wheel, st5) == kRouteViewer);
        KeyRouteInput rec_up = alt_up;
        rec_up.recording = true;
        CHECK(RouteViewerKey(b, rec_up, st5) == kRouteViewerSys);
        CHECK(st5.released_vk == vk::kMenu);  // the recording rule
        CHECK(st5.alt_wheel_taken);           // not consumed by the Alt + wheel rule
        // A plain KeyUp of Alt after Alt + wheel: the viewer's, nothing to swallow.
        KeyRouteState st6;
        CHECK(RouteViewerKey(b, wheel, st6) == kRouteViewer);
        KeyRouteInput alt_keyup = alt_up;
        alt_keyup.msg = KeyMsg::KeyUp;
        CHECK(RouteViewerKey(b, alt_keyup, st6) == kRouteViewer);
        CHECK(st6.released_vk == 0 && !st6.alt_wheel_taken);
    }

    // Spec 11-fb-14: Shift + wheel over the viewer's UI (the strip's scroll) is the viewer's.
    {
        const ShortcutBindings b = DefaultShortcutBindings();
        KeyRouteState st;
        KeyRouteInput wheel;
        wheel.msg = KeyMsg::Wheel;
        wheel.shift = true;
        wheel.imgui_mouse = true;
        CHECK(RouteViewerKey(b, wheel, st) == kRouteViewer);
        CHECK(!st.alt_wheel_taken);  // Shift's release has nothing to swallow
        KeyRouteInput shift_up;
        shift_up.msg = KeyMsg::KeyUp;
        shift_up.key = vk::kShift;
        CHECK(RouteViewerKey(b, shift_up, st) == kRouteReaper);
        KeyRouteInput off_ui = wheel;
        off_ui.imgui_mouse = false;
        CHECK(RouteViewerKey(b, off_ui, st) == kRouteReaper);
        // Alt + Shift + wheel: Alt's rule (its release swallowed).
        KeyRouteInput both = wheel;
        both.alt = true;
        CHECK(RouteViewerKey(b, both, st) == kRouteViewer);
        CHECK(st.alt_wheel_taken);
    }

    // Spec 11-fb-17: a text field takes Ctrl and the editing keys (Ctrl+A selects all); the
    // characters of the viewer's keys are the viewer's (no beep from the docker dialog).
    {
        const ShortcutBindings b = DefaultShortcutBindings();
        auto ev = [](KeyMsg m, unsigned key, bool text, bool ctrl = false, bool shift = false) {
            KeyRouteInput i;
            i.msg = m;
            i.key = key;
            i.text_input = text;
            i.ctrl = ctrl;
            i.shift = shift;
            i.video_view = true;
            return i;
        };
        KeyRouteState st;
        // Text input: Ctrl down, Ctrl+A / C / V / X / Z with their chars and releases.
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, vk::kControl, true, true), st) == kRouteViewer);
        const unsigned ctrl_keys[] = {'A', 'C', 'V', 'X', 'Z'};
        for (unsigned k : ctrl_keys) {
            CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, k, true, true), st) == kRouteViewer);
            CHECK(RouteViewerKey(b, ev(KeyMsg::Char, k - 'A' + 1, true, true), st) == kRouteViewer);  // 0x01..
            CHECK(RouteViewerKey(b, ev(KeyMsg::KeyUp, k, true, true), st) == kRouteViewer);
        }
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyUp, vk::kControl, true), st) == kRouteViewer);
        // Home / End, Shift+Left.
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, vk::kHome, true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyUp, vk::kHome, true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, vk::kEnd, true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyUp, vk::kEnd, true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, vk::kShift, true, false, true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, vk::kLeft, true, false, true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyUp, vk::kLeft, true, false, true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyUp, vk::kShift, true), st) == kRouteViewer);
        // Text input: Space and a key the viewer has no action for are the field's.
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, vk::kSpace, true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::Char, ' ', true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyUp, vk::kSpace, true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, 'K', true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::Char, 'k', true), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyUp, 'K', true), st) == kRouteViewer);
        // No text input: Ctrl+A and Space stay REAPER's.
        st = KeyRouteState{};
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, vk::kControl, false, true), st) == kRouteReaper);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, 'A', false, true), st) == kRouteReaper);
        CHECK(RouteViewerKey(b, ev(KeyMsg::Char, 0x01, false, true), st) == kRouteReaper);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyUp, 'A', false, true), st) == kRouteReaper);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, vk::kSpace, false), st) == kRouteReaper);
        CHECK(RouteViewerKey(b, ev(KeyMsg::Char, ' ', false), st) == kRouteReaper);
        CHECK(!ViewerTakesChar(st, false, false));  // Space's char keeps its old path
        // The char of a claimed action key (C in Video view) is the viewer's, and the window
        // takes it (ViewerTakesChar) until the key's release.
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyDown, 'C', false), st) == kRouteViewer);
        CHECK(ViewerTakesChar(st, false, false));
        CHECK(RouteViewerKey(b, ev(KeyMsg::Char, 'c', false), st) == kRouteViewer);
        CHECK(RouteViewerKey(b, ev(KeyMsg::KeyUp, 'C', false), st) == kRouteViewer);
        CHECK(!ViewerTakesChar(st, false, false));
        // ViewerTakesChar truth table.
        KeyRouteState none, claimed;
        claimed.claimed_vk = 'C';
        CHECK(!ViewerTakesChar(none, false, false));
        CHECK(ViewerTakesChar(none, true, false));
        CHECK(ViewerTakesChar(none, false, true));
        CHECK(ViewerTakesChar(none, true, true));
        CHECK(ViewerTakesChar(claimed, false, false));
        CHECK(ViewerTakesChar(claimed, true, false));
        CHECK(ViewerTakesChar(claimed, false, true));
        CHECK(ViewerTakesChar(claimed, true, true));
    }

    // ---- Story 10-4: Tagging view's E and Del ---------------------------------------------
    {
        const ShortcutBindings b = DefaultShortcutBindings();
        CHECK(b[kShortcutTagAddEvent] == Key('E'));
        CHECK(b[kShortcutTagDelEvent] == Key(vk::kDelete));
        CHECK(kShortcutTable[kShortcutTagDelEvent].context == ShortcutContext::TaggingView);
        // Del: the shot in Video view (unchanged), the event in Tagging view, REAPER's in RAV view.
        CHECK(ShortcutActionFor(b, vk::kDelete, false, false, false, true, false) == kShortcutDeleteShot);
        CHECK(ShortcutActionFor(b, vk::kDelete, false, false, false, false, true) == kShortcutTagDelEvent);
        CHECK(ShortcutActionFor(b, vk::kDelete, false, false, false, false, false) == -1);
        CHECK(ShortcutActionFor(b, 'E', false, false, false, false, true) == kShortcutTagAddEvent);
        CHECK(ShortcutActionFor(b, 'E', false, false, false, true, false) == -1);   // Video view: REAPER's
        CHECK(ShortcutActionFor(b, 'E', false, false, false, false, false) == -1);  // RAV view: REAPER's
        // The shared default is no clash (the two views are never on together).
        ShortcutBindings d = DefaultShortcutBindings();
        CHECK(SanitizeShortcuts(d).empty());
        CHECK(ShortcutOwner(d, Key(vk::kDelete), kShortcutDeleteShot) == -1);
        CHECK(ShortcutOwner(d, Key(vk::kDelete), kShortcutTagDelEvent) == -1);
        // ...but an Anywhere action cannot take E, and a Tagging action cannot take V.
        int owner = -1;
        CHECK(ShortcutRecordStep(d, kShortcutToggleView, 'E', false, false, false, &owner) == RecordOutcome::Conflict);
        CHECK(owner == kShortcutTagAddEvent);
        CHECK(ShortcutRecordStep(d, kShortcutTagAddEvent, 'V', false, false, false, &owner) == RecordOutcome::Conflict);
        CHECK(owner == kShortcutToggleView);
        // Cut (Video view) may take E: they are never on together.
        CHECK(ShortcutRecordStep(d, kShortcutCut, 'E', false, false, false) == RecordOutcome::Assigned);
        CHECK(SanitizeShortcuts(d).empty());
        // Resetting Delete shot leaves Tagging view's Delete alone.
        ShortcutBindings r = DefaultShortcutBindings();
        r[kShortcutDeleteShot] = Key(vk::kBack);
        const std::vector<int> ch = ResetShortcut(r, kShortcutDeleteShot);
        CHECK(ch.size() == 1 && r[kShortcutTagDelEvent] == Key(vk::kDelete));
        // Routing: Del reaches the viewer in Tagging view.
        KeyRouteState st;
        KeyRouteInput del;
        del.msg = KeyMsg::KeyDown;
        del.key = vk::kDelete;
        CHECK(RouteViewerKey(b, del, st) == kRouteReaper);
        del.tagging_view = true;
        CHECK(RouteViewerKey(b, del, st) == kRouteViewer);
        CHECK(FormatShortcut(b[kShortcutTagAddEvent]) == "E");
    }

    // Story 6-5-10 -- the clip list: L in every view, rebindable, clashing with no default.
    {
        const ShortcutBindings b = DefaultShortcutBindings();
        CHECK(b[kShortcutClipList] == Key('L'));
        CHECK(std::string(kShortcutTable[kShortcutClipList].id) == "clip_list");
        CHECK(kShortcutTable[kShortcutClipList].context == ShortcutContext::Anywhere);
        CHECK(ShortcutActionFor(b, 'L', false, false, false, false, false) == kShortcutClipList);
        CHECK(ShortcutActionFor(b, 'L', false, false, false, true, false) == kShortcutClipList);
        CHECK(ShortcutActionFor(b, 'L', false, false, false, false, true) == kShortcutClipList);
        CHECK(ShortcutActionFor(b, 'L', true, false, false, false, false) == -1);  // Ctrl+L stays REAPER's
        for (int i = 0; i < kShortcutCount; ++i)
            if (i != kShortcutClipList) CHECK(!(b[i] == Key('L')));
        ShortcutBindings d = b;
        CHECK(SanitizeShortcuts(d).empty());
        int owner = -1;
        CHECK(ShortcutRecordStep(d, kShortcutClipList, 'P', false, false, false, &owner) == RecordOutcome::Conflict);
        CHECK(owner == kShortcutTogglePanel);
        CHECK(ShortcutRecordStep(d, kShortcutClipList, 'K', false, false, false) == RecordOutcome::Assigned);
        CHECK(ResetShortcut(d, kShortcutClipList).size() == 1 && d[kShortcutClipList] == Key('L'));
    }

    // Story 6-5-9 -- full screen: F11 (anywhere, rebindable), Esc is the viewer's while full
    // screen, and a key it does not take goes to REAPER's main shortcuts (-666), Alt ones excepted.
    {
        const ShortcutBindings b = DefaultShortcutBindings();
        CHECK(b[kShortcutFullScreen] == Key(vk::kF11));
        CHECK(kShortcutTable[kShortcutFullScreen].context == ShortcutContext::Anywhere);
        CHECK(ShortcutActionFor(b, vk::kF11, false, false, false, false, false) == kShortcutFullScreen);
        CHECK(ShortcutActionFor(b, vk::kF11, false, false, false, true, false) == kShortcutFullScreen);
        CHECK(ShortcutActionFor(b, vk::kF11, false, false, false, false, true) == kShortcutFullScreen);
        CHECK(FormatShortcut(b[kShortcutFullScreen]) == "F11");
        ShortcutBindings d = DefaultShortcutBindings();
        CHECK(SanitizeShortcuts(d).empty());
        // A custom key for it: V and E are taken (Anywhere overlaps every context), F is free.
        int owner = -1;
        CHECK(ShortcutRecordStep(d, kShortcutFullScreen, 'V', false, false, false, &owner) == RecordOutcome::Conflict);
        CHECK(owner == kShortcutToggleView);
        CHECK(ShortcutRecordStep(d, kShortcutFullScreen, 'E', false, false, false, &owner) == RecordOutcome::Conflict);
        CHECK(ShortcutRecordStep(d, kShortcutFullScreen, 'F', false, false, false) == RecordOutcome::Assigned);
        CHECK(DecodeShortcut(EncodeShortcut(Key(vk::kF11)).c_str(), &d[kShortcutFullScreen]) &&
              d[kShortcutFullScreen] == Key(vk::kF11));

        KeyRouteState st;
        KeyRouteInput f11;
        f11.msg = KeyMsg::KeyDown;
        f11.key = vk::kF11;
        CHECK(RouteViewerKey(b, f11, st) == kRouteViewer);  // docked: F11 is the viewer's (not REAPER's)
        f11.msg = KeyMsg::KeyUp;
        CHECK(RouteViewerKey(b, f11, st) == kRouteViewer);  // its release follows it
        CHECK(st.claimed_vk == 0);

        KeyRouteInput esc;
        esc.msg = KeyMsg::KeyDown;
        esc.key = vk::kEscape;
        CHECK(RouteViewerKey(b, esc, st) == kRouteReaper);  // docked: Esc stays REAPER's
        esc.full_screen = true;
        CHECK(RouteViewerKey(b, esc, st) == kRouteViewer);  // full screen: it leaves
        esc.msg = KeyMsg::KeyUp;
        CHECK(RouteViewerKey(b, esc, st) == kRouteViewer);

        KeyRouteInput space;
        space.msg = KeyMsg::KeyDown;
        space.key = vk::kSpace;
        CHECK(RouteViewerKey(b, space, st) == kRouteReaper);  // docked: unchanged
        space.full_screen = true;
        CHECK(RouteViewerKey(b, space, st) == kRouteReaperMain);  // full screen: REAPER plays
        space.msg = KeyMsg::KeyUp;
        CHECK(RouteViewerKey(b, space, st) == kRouteReaperMain);
        KeyRouteInput ctrl_s;
        ctrl_s.msg = KeyMsg::KeyDown;
        ctrl_s.key = 'S';
        ctrl_s.ctrl = true;
        ctrl_s.full_screen = true;
        CHECK(RouteViewerKey(b, ctrl_s, st) == kRouteReaperMain);
        KeyRouteInput alt_x;
        alt_x.msg = KeyMsg::SysKeyDown;
        alt_x.key = 'X';
        alt_x.alt = true;
        alt_x.full_screen = true;
        CHECK(RouteViewerKey(b, alt_x, st) == kRouteReaper);  // Alt: no menu bar behind the window
        KeyRouteInput ch;
        ch.msg = KeyMsg::Char;
        ch.key = ' ';
        ch.full_screen = true;
        CHECK(RouteViewerKey(b, ch, st) == kRouteReaper);  // characters keep their old path
        // A text field still takes every key, Esc and Space included.
        KeyRouteInput typed = space;
        typed.msg = KeyMsg::KeyDown;
        typed.text_input = true;
        CHECK(RouteViewerKey(b, typed, st) == kRouteViewer);
        typed.msg = KeyMsg::KeyUp;
        CHECK(RouteViewerKey(b, typed, st) == kRouteViewer);
        // V still switches views full screen.
        KeyRouteInput v;
        v.msg = KeyMsg::KeyDown;
        v.key = 'V';
        v.full_screen = true;
        CHECK(RouteViewerKey(b, v, st) == kRouteViewer);
    }

    // 13-60 -- a row linked to a REAPER action runs on that action's keys; it has no key of its own.
    {
        ShortcutBindings b = DefaultShortcutBindings();
        bool             linked[kShortcutCount] = {};
        ShortcutKeyLists lk;
        // Cut (Video view) linked to an action REAPER gives X and C: both run Cut (no own key while linked).
        linked[kShortcutCut] = true;
        b[kShortcutCut] = KeyBinding{};
        lk[kShortcutCut] = {Key('X'), Key('C'), Key(vk::kEscape)};  // Esc is never a key: dropped
        ShortcutKeymap m = ResolveShortcutKeys(b, lk, linked);
        CHECK(m.keys[kShortcutCut].size() == 2);
        CHECK(ShortcutActionFor(m, 'X', false, false, false, true) == kShortcutCut);
        CHECK(ShortcutActionFor(m, 'C', false, false, false, true) == kShortcutCut);
        CHECK(ShortcutActionFor(m, 'X', false, false, false, false) == -1);  // its context: Video view only
        CHECK(ShortcutKeyOwner(m, Key('X'), -1) == kShortcutCut);
        // A key RAV gives another row (V: Next view, Anywhere) wins over the linked action's.
        lk[kShortcutCut].push_back(Key('V'));
        m = ResolveShortcutKeys(b, lk, linked);
        CHECK(ShortcutActionFor(m, 'V', false, false, false, true) == kShortcutToggleView);
        CHECK(m.shadowed[kShortcutCut].size() == 1 && m.shadowed[kShortcutCut][0] == Key('V'));
        // Two linked rows on one key: the first in the table keeps it; contexts that never meet share it.
        linked[kShortcutTagAddEvent] = true;
        b[kShortcutTagAddEvent] = KeyBinding{};
        lk[kShortcutTagAddEvent] = {Key('X')};  // Tagging view vs Video view: no clash
        linked[kShortcutTogglePanel] = true;
        b[kShortcutTogglePanel] = KeyBinding{};
        lk[kShortcutTogglePanel] = {Key('X', true)};
        lk[kShortcutFullScreen] = {Key('X', true)};  // not linked: ignored
        m = ResolveShortcutKeys(b, lk, linked);
        CHECK(ShortcutActionFor(m, 'X', false, false, false, false, true) == kShortcutTagAddEvent);
        CHECK(ShortcutActionFor(m, 'X', false, false, false, true, false) == kShortcutCut);
        CHECK(ShortcutActionFor(m, 'X', true, false, false, false) == kShortcutTogglePanel);
        CHECK(ShortcutActionFor(m, vk::kF11, false, false, false, false) == kShortcutFullScreen);  // its own key
        linked[kShortcutPasteCamera] = true;  // Ctrl+V's row, linked to Ctrl+X too: Toggle panel (earlier) keeps it
        b[kShortcutPasteCamera] = KeyBinding{};
        lk[kShortcutPasteCamera] = {Key('X', true)};
        m = ResolveShortcutKeys(b, lk, linked);
        CHECK(ShortcutActionFor(m, 'X', true, false, false, true) == kShortcutTogglePanel);
        CHECK(m.shadowed[kShortcutPasteCamera].size() == 1);
        // Routing: the hook hands a linked key to the viewer, and its release follows it.
        KeyRouteState st;
        KeyRouteInput x;
        x.msg = KeyMsg::KeyDown;
        x.key = 'X';
        x.video_view = true;
        CHECK(RouteViewerKey(m, x, st) == kRouteViewer);
        x.msg = KeyMsg::KeyUp;
        CHECK(RouteViewerKey(m, x, st) == kRouteViewer);
        x.msg = KeyMsg::KeyDown;
        x.video_view = false;  // RAV view: Cut is not on, X goes to REAPER
        CHECK(RouteViewerKey(m, x, st) == kRouteReaper);
        // The plain bindings' overload is unchanged (no link).
        CHECK(RouteViewerKey(DefaultShortcutBindings(), x, st) == kRouteReaper);
    }
    // 13-60 -- load / reset rules leave a linked row alone: no default given back while it is linked.
    {
        ShortcutBindings b = DefaultShortcutBindings();
        bool             linked[kShortcutCount] = {};
        linked[kShortcutCut] = true;
        b[kShortcutCut] = KeyBinding{};
        CHECK(SanitizeShortcuts(b, nullptr, nullptr, linked).empty());
        CHECK(b[kShortcutCut].vk == 0);
        CHECK(RebindFreedDefaults(b, linked).empty());
        CHECK(b[kShortcutCut].vk == 0);
        // Without the flags (unlinked) the old rules give its default back.
        CHECK(Contains(RebindFreedDefaults(b), kShortcutCut));
        CHECK(b[kShortcutCut] == Key('C'));
        // A linked row's own key C is now free: another row can record it.
        b = DefaultShortcutBindings();
        b[kShortcutCut] = KeyBinding{};
        int owner = -1;
        CHECK(ShortcutRecordStep(b, kShortcutDeleteShot, 'C', false, false, false, &owner) == RecordOutcome::Assigned);
    }

    // 13-60b: every row has its own action in REAPER's list (a row on REAPER follows its keys): a distinct
    // name, REAPER's id "_" + it, a text naming RAV; full screen keeps story 6-5-9's action.
    {
        for (int i = 0; i < kShortcutCount; ++i) {
            CHECK(std::strncmp(kShortcutTable[i].action, "RAV_", 4) == 0);
            CHECK(std::strncmp(kShortcutTable[i].action_desc, "RAV: ", 5) == 0);
            CHECK(ShortcutReaperActionId(i) == std::string("_") + kShortcutTable[i].action);
            for (int j = 0; j < i; ++j) CHECK(std::strcmp(kShortcutTable[i].action, kShortcutTable[j].action) != 0);
        }
        CHECK(std::strcmp(kShortcutTable[kShortcutFullScreen].action, "RAV_VIEWER_FULLSCREEN") == 0);
        CHECK(ShortcutReaperActionId(-1).empty() && ShortcutReaperActionId(kShortcutCount).empty());
        // A row on REAPER is stored as "reaper": never a key, never a link.
        KeyBinding k;
        CHECK(!DecodeShortcut(kShortcutFollowsReaper, &k));
        // On REAPER, a row is linked to its own action: its keys (here F2 given in REAPER) run it, and a key
        // REAPER gives it that another row holds as its own stays that row's.
        ShortcutBindings b = DefaultShortcutBindings();
        b[kShortcutCut] = KeyBinding{};  // on REAPER: no key of its own
        bool linked[kShortcutCount] = {};
        linked[kShortcutCut] = true;
        ShortcutKeyLists lk;
        lk[kShortcutCut] = {Key(vk::kF1 + 1), Key('P')};
        const ShortcutKeymap m = ResolveShortcutKeys(b, lk, linked);
        CHECK(ShortcutActionFor(m, vk::kF1 + 1, false, false, false, true) == kShortcutCut);
        CHECK(ShortcutActionFor(m, vk::kF1 + 1, false, false, false, false) == -1);  // Video only, as its own key
        CHECK(ShortcutActionFor(m, 'P', false, false, false, true) == kShortcutTogglePanel);
        CHECK(m.shadowed[kShortcutCut].size() == 1);
        CHECK(Contains(RebindFreedDefaults(b, linked), kShortcutCut) == false);
    }

    if (g_fails == 0) std::printf("shortcuts: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
