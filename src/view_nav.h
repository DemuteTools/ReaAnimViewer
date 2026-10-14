// SPDX-License-Identifier: MIT
//
// Story 13-45 -- one way to scroll and zoom every timeline in RAV (the Sound Edit window, a Blend's
// lanes, the Video view's shot strip, the Tagging view's strip). Six view actions, each a row of the
// Shortcuts popup's "View: scroll and zoom" group (option A of the mockup
// maquette-navigation-vues-raccourcis.html): linked to REAPER (the default: it follows the user's
// binding of REAPER's matching action, read from reaper-kb.ini, live) or the user's Own gesture (a
// key, or the wheel / horizontal wheel, with Ctrl / Shift / Alt).
//
// The top half is pure (no REAPER, no Windows, no ImGui): the table, the resolution of the user's
// choices against his REAPER bindings into one map (gesture -> action), the step a frame's input
// makes, the recording rule and the ExtState encoding. tests/view_nav_test.cpp includes it as is.
// The bottom half is the live state (src/view_nav_live.cpp): reaper-kb.ini watched, the choices in
// ExtState, the recording session, and the one input reader every view calls.

#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "reaper_view_keys.h"

namespace rav {

// ---- the table ------------------------------------------------------------------------

enum ViewNavRow : int {
    kNavScrollH = 0,
    kNavScrollV = 1,
    kNavZoomH   = 2,
    kNavZoomV   = 3,
    kNavZoomIn  = 4,
    kNavZoomOut = 5,
    kNavRowCount = 6,
};

struct ViewNavDef {
    const char* id;           // ExtState key suffix: viewnav.<id>
    const char* label;        // the row's label, "Already used by <label>"
    const char* reaper_name;  // the REAPER action it is linked to (Main section)
    int         reaper_cmd;   // ...and its command id
    bool        wheel_ok;     // a wheel gesture can be its own (the zoom in / out rows take keys only)
};

inline const ViewNavDef kViewNavTable[kNavRowCount] = {
    {"scroll_h", "Scroll horizontally", "View: Scroll horizontally (MIDI CC relative/mousewheel)", 988, true},
    {"scroll_v", "Scroll vertically", "View: Scroll vertically (MIDI CC relative/mousewheel)", 989, true},
    {"zoom_h", "Zoom horizontally", "View: Zoom horizontally (MIDI CC relative/mousewheel)", 990, true},
    {"zoom_v", "Zoom vertically", "View: Zoom vertically (MIDI CC relative/mousewheel)", 1000, true},
    {"zoom_in", "Zoom in", "View: Zoom in horizontal", 1012, false},
    {"zoom_out", "Zoom out", "View: Zoom out horizontal", 1011, false},
};

// The row of a REAPER wheel action (-1 for None).
int ViewNavRowOf(ViewWheelAction a);

// ---- gestures -------------------------------------------------------------------------

enum class GestureKind { None, Key, Wheel, HWheel };

struct ViewGesture {
    GestureKind kind = GestureKind::None;
    int         code = 0;         // Key: a Win32 virtual key, or a character when is_char
    bool        is_char = false;
    int         mods = 0;         // kViewMod* bits (reaper_view_keys.h)
    bool operator==(const ViewGesture& o) const
    {
        return kind == o.kind && mods == o.mods && (kind != GestureKind::Key || (code == o.code && is_char == o.is_char));
    }
    bool operator!=(const ViewGesture& o) const { return !(*this == o); }
};

ViewGesture WheelGesture(bool horizontal, int mods);
ViewGesture KeyGesture(int vk, int mods);

// "Ctrl+Shift+Alt+Wheel", "Alt+H-wheel", "Ctrl+Up" (FormatShortcut's modifier order). A key's name:
// ShortcutKeyName (shortcuts.h), else `fallback` (the keyboard layout's name). "" for None.
std::string FormatViewGesture(const ViewGesture& g, std::string (*fallback)(unsigned) = nullptr);

// ---- the user's choices and the resolved map -------------------------------------------

struct ViewNavChoice {
    bool        own = false;  // false: linked to REAPER (the default)
    ViewGesture g;            // Own: the user's gesture (None = not set yet)
    bool operator==(const ViewNavChoice& o) const { return own == o.own && g == o.g; }
};
using ViewNavChoices = std::array<ViewNavChoice, kNavRowCount>;

struct ViewNavWheel {
    int  row = -1;  // the action (-1 = none)
    bool reversed = false;
    bool page = false;
};

// Every gesture of the six actions as they stand: what each wheel (by modifiers) and key runs.
struct ViewNavMap {
    ViewNavWheel wheel[8], hwheel[8];
    std::vector<std::pair<ViewGesture, int>> keys;  // key gesture -> row
};

// A linked row takes REAPER's gestures for its action (all of them); an Own row only its own
// gesture, which it takes from any linked row that had it (the user's choice wins).
ViewNavMap ResolveViewNav(const ViewKeymap& reaper, const ViewNavChoices& choices);

// The gestures that run `row` (the wheel ones first, by modifiers, then the keys).
std::vector<ViewGesture> ViewNavGestures(const ViewNavMap& m, int row);
// The gestures REAPER binds to that row's action (what a linked row would have).
std::vector<ViewGesture> ReaperViewGestures(const ViewKeymap& reaper, int row);

// The row a gesture runs now, or -1.
int ViewNavOwner(const ViewNavMap& m, const ViewGesture& g);
// The row a virtual key with these modifiers runs (key gestures that are characters aside), or -1.
int ViewNavKeyRow(const ViewNavMap& m, int vk, int mods);
// The row a typed character runs (a key REAPER binds as a character, e.g. "+"), or -1. Shift is part
// of the character, so only Ctrl and Alt must match.
int ViewNavCharRow(const ViewNavMap& m, int ch, int mods);

// ---- one frame's input -> the view's moves ---------------------------------------------

// Signs: zoom > 0 = in, scroll > 0 = right (later) / down. Units: wheel notches (fractions for a
// trackpad); *_pages = whole views (the "one page" REAPER actions).
struct ViewNavStep {
    double zoom_h = 0.0, scroll_h = 0.0, scroll_h_pages = 0.0;
    double zoom_v = 0.0, scroll_v = 0.0, scroll_v_pages = 0.0;
    bool any_h() const { return zoom_h != 0.0 || scroll_h != 0.0 || scroll_h_pages != 0.0; }
    bool any_v() const { return zoom_v != 0.0 || scroll_v != 0.0 || scroll_v_pages != 0.0; }
};

// `wheel` / `hwheel`: Dear ImGui's io.MouseWheel (> 0 up) / io.MouseWheelH (> 0 left), `mods`
// held; `key_rows`: the rows whose key was pressed this frame. The wheel up and the horizontal
// wheel left both mean "back / in": scroll left or up, zoom in (a reversed command the other
// way). A key press: zoom in / out by two notches (1.25 x 1.25), scroll by one notch forward.
ViewNavStep ViewNavStepFor(const ViewNavMap& m, int mods, float wheel, float hwheel, const std::vector<int>& key_rows);

// Does a wheel with these modifiers run an action? (Then the view takes it from ImGui's scrolling.)
bool ViewNavWheelBound(const ViewNavMap& m, int mods, bool horizontal);

// A timeline inside a scrolling panel (a Blend's lanes in the Sound panel) leaves the wheel Dear
// ImGui scrolls panels with (plain or Shift, no Ctrl / Alt) to the panel until the user works in it
// (zoomed in, or clicked): true for those modifiers.
bool ViewNavWheelYields(int mods);

// ---- reaper-kb.ini re-read only once it stopped changing -----------------------------------

// The file's stamp (exists, date, size) as last applied, and a newer one waiting to settle: REAPER
// may be writing it, a half-written file is never read.
struct KbStamp {
    bool               exists = false;
    long long          time = 0;
    unsigned long long size = 0;
    bool operator==(const KbStamp& o) const { return exists == o.exists && time == o.time && size == o.size; }
    bool operator!=(const KbStamp& o) const { return !(*this == o); }
};
struct KbWatch {
    bool    applied_once = false;
    KbStamp applied;
    bool    waiting = false;
    KbStamp seen;
    double  seen_at = 0.0;
};
// One check at `now` (seconds) with the file's stamp: true when it must be read now -- the first time,
// or a stamp other than the applied one seen unchanged for at least `settle` seconds. The caller reads
// it, then calls KbApplied.
bool KbShouldRead(KbWatch& w, const KbStamp& now_stamp, double now, double settle = 1.0);
void KbApplied(KbWatch& w, const KbStamp& stamp);

// ---- recording an Own gesture -----------------------------------------------------------

enum class NavRecordOutcome {
    Ignored,   // not a gesture this row takes (a wheel for Zoom in / out): still waiting
    Conflict,  // another row runs it now (*owner): the user confirms (Keep) or not
    Assigned,  // free (or this row's already): assign it
};

NavRecordOutcome ViewNavRecordStep(const ViewNavMap& m, int row, const ViewGesture& g, int* owner = nullptr);

// Row becomes Own with gesture g; any other Own row that had it loses it (Own, not set). Linked rows
// lose it by themselves (ResolveViewNav).
void ViewNavAssign(ViewNavChoices& c, int row, const ViewGesture& g);

// ---- ExtState encoding ------------------------------------------------------------------

// Linked = "" (no entry); Own not set = "own"; Own = "own:" + the gesture ("Ctrl+Shift+Alt+" then
// "Wheel", "HWheel", a decimal virtual key, or "char" + a decimal character).
std::string EncodeViewNavChoice(const ViewNavChoice& c);
// False when the text is not valid (then *out is untouched). "" decodes to linked.
bool DecodeViewNavChoice(const char* text, ViewNavChoice* out);

// The tip a timeline shows: "Zoom: Wheel · Scroll: Alt+Wheel" (the first gesture of each), "" when
// neither has one.
std::string ViewNavHint(const ViewNavMap& m, std::string (*fallback)(unsigned) = nullptr);

// ---- live state (src/view_nav_live.cpp, REAPER + Windows + Dear ImGui) ---------------------

// The map in force: the user's choices (ExtState, read once) over his REAPER bindings (reaper-kb.ini,
// read again when the file changes: its date and size checked at most once a second).
const ViewNavMap& CurrentViewNav();
const ViewKeymap& CurrentReaperViewKeys();
const ViewNavChoices& CurrentViewNavChoices();
// Seconds since reaper-kb.ini was last read (-1 = never: not found, REAPER's defaults).
double ViewNavKbAge();

// The Shortcuts popup's gestures on a row: linked (right-click) / Own (the toggle). Saved at once
// (ExtState, no undo point: a viewer setting, like the keys).
void SetViewNavLinked(int row);
void SetViewNavOwn(int row);

// The gesture's label, with the keyboard layout's key names.
std::string ViewGestureLabel(const ViewGesture& g);
// A row's gestures now, for a tip: "-, Down", "none: see Shortcuts".
std::string ViewNavRowText(int row);
// The tip a timeline shows (ViewNavHint with the layout's key names).
std::string ViewNavTip();

// Recording (double-click a row): the next key or wheel. While it runs, the accelerator hook hands
// every key and wheel to the viewer (ShortcutRecordingId() >= 0 covers it).
void BeginViewNavRecording(int row);
void CancelViewNavRecording();
int  ViewNavRecordingRow();  // -1 when not recording
void ViewNavRecordKey(unsigned vk, bool ctrl, bool shift, bool alt);
void ViewNavRecordWheel(bool horizontal, bool ctrl, bool shift, bool alt);
// The gesture waiting for Keep (a conflict), its owner row; false when none.
bool ViewNavPending(ViewGesture* g, int* owner);
void ViewNavKeepPending();
// Why the last gesture was refused ("" = none): a RAV key's, or the wheel on a key-only row.
const char* ViewNavRefusal();

// ---- the one input reader every timeline calls ----------------------------------------

// This frame's moves for a view: the wheel when `wheel_on` (the mouse over it), the keys when
// `keys_on` (it is hovered, or has the focus). A view hovered marks itself (the accelerator hook then
// hands it its zoom keys: ViewNavTakesKey) and, when the wheel runs an action, takes the wheel from
// Dear ImGui's own scrolling (`owner_id`, the view's ImGui id; 0 = no claim). `panel_scroll_first`:
// the plain / Shift wheel stays the panel's (ViewNavWheelYields).
// The claim lags one frame (ImGui reads a key's owner as the frame starts): the first notch over a
// timeline just entered may still scroll the panel. Accepted (13-45 review).
ViewNavStep ViewNavInput(bool wheel_on, bool keys_on, unsigned owner_id = 0, bool panel_scroll_first = false);
// A key a hovered timeline runs (the accelerator hook claims it for the viewer).
bool ViewNavTakesKey(unsigned vk, bool ctrl, bool shift, bool alt);
// The row a key down runs (its virtual key, or the character it types on this keyboard layout), -1.
int ViewNavRowForKeyDown(unsigned vk, bool ctrl, bool shift, bool alt);
// Does a timeline under the mouse (last frame) run this wheel? The accelerator hook then keeps it from
// REAPER (a Ctrl+wheel RAV does not use goes on to REAPER).
bool ViewNavClaimsWheel(bool horizontal, bool ctrl, bool shift, bool alt);

}  // namespace rav
