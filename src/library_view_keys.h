// SPDX-License-Identifier: MIT
//
// 13-48 (13-7a review G-d): the Library view's key claim as a pure predicate (host-tested by
// tests/library_tree_nav_test.cpp). LibraryViewTakesKey (library_view_ui.cpp) fills the state from
// the view's Win32 focus and the panels; the node panel's own key (E) is asked before it.

#pragma once

namespace rav {

struct LibraryKeyState {
    bool focused = false;             // the Library view's window has the keyboard focus
    bool media_takes_arrows = false;  // the Media Explorer is expanded and its list takes Up / Down
    bool tree_takes_arrows = false;   // the node tree has the focus (its rows walk with the arrows)
};

// Decision 7 (13-7a): a key down goes to the viewer only while the view is focused, never with Alt:
// Delete, F2, Space, Esc; Ctrl+B / D / X / V (never with Shift); Up / Down for the Media Explorer,
// the four arrows for the tree. Ctrl+Z and everything else stay REAPER's.
inline bool LibraryViewClaimsKey(const LibraryKeyState& s, unsigned vk, bool ctrl, bool shift, bool alt)
{
    if (!s.focused || alt) return false;
    constexpr unsigned kDelete = 0x2E, kF2 = 0x71, kSpace = 0x20, kEscape = 0x1B;
    constexpr unsigned kLeft = 0x25, kUp = 0x26, kDown = 0x28;
    if (!ctrl && !shift && (vk == kUp || vk == kDown) && s.media_takes_arrows) return true;
    if (!ctrl && !shift && vk >= kLeft && vk <= kDown && s.tree_takes_arrows) return true;
    if (ctrl) return !shift && (vk == 'B' || vk == 'D' || vk == 'X' || vk == 'V');
    return vk == kDelete || vk == kF2 || vk == kSpace || vk == kEscape;
}

// 13-60 review: a key the Library view uses when it has the focus (any panel: the claim above with
// every panel taking its keys, plus the Sound panel's and Edit's E; 13-63: S is no longer theirs, the
// split is a Shortcuts row). A RAV shortcut on such a key
// does not run while the Library view takes it (the focused view wins); the Link modal warns about it.
inline bool LibraryViewUsesKey(unsigned vk, bool ctrl, bool shift, bool alt)
{
    LibraryKeyState all;
    all.focused = all.media_takes_arrows = all.tree_takes_arrows = true;
    if (LibraryViewClaimsKey(all, vk, ctrl, shift, alt)) return true;
    return !ctrl && !shift && !alt && vk == 'E';
}

}  // namespace rav
